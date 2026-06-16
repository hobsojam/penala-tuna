#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
#include <cmath>
#include <algorithm>
#include <string>

#include "notes.h"
#include "tone.h"
#include "capture.h"
#include "pitch.h"

// ── chromatic → diatonic position ────────────────────────────────────────────
// Matches the note names in notes.cpp (flats for Eb Ab Bb, sharps for C# F#).
// PC: C  C# D  Eb E  F  F# G  Ab A  Bb B
static const int  CHROM_DIAT[12] = { 0, 0, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6 };
static const bool IS_FLAT[12]    = { 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0 };
static const bool IS_SHARP[12]   = { 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 };
// Piano strip key layout: white-key index within octave (-1 = black key)
static const int PC_TO_WHITE[12]   = {  0,-1, 1,-1, 2, 3,-1, 4,-1, 5,-1, 6 };
// For black keys: white-key index immediately to their left, within octave
static const int PC_BLACK_LEFT[12] = { -1, 0,-1, 1,-1,-1, 3,-1, 4,-1, 5,-1 };

// ── layout constants ──────────────────────────────────────────────────────────
static const int CX         = 820;
static const int CY         = 750;

static const int STAFF_LEFT = 145;
static const int STAFF_RIGHT= 755;
static const int STAFF_TOP  = 160;
static const int LINE_GAP   = 22;
static const int STEP       = 11;
static const int STAFF_BOT  = STAFF_TOP + 4 * LINE_GAP; // 248

static const int TARGET_X   = 370;
static const int DETECTED_X = 555;
static const int HEAD_W     = 13;
static const int HEAD_H     = 9;

static const int INFO_Y1    = 306;
static const int INFO_Y2    = 332;
static const int INFO_Y3    = 360;   // mode-specific info row
static const int CENTS_Y    = 390;
static const int CENTS_H    = 14;

static const int TRACE_Y    = 420;   // pitch-history strip
static const int TRACE_H    = 148;

static const int HARM_TOP   = 582;   // harmonic bars
static const int HARM_H     = 65;

static const int PIANO_LO   = 48;    // C3 — lowest key shown
static const int PIANO_HI   = 71;    // B4 — highest key shown
static const int PIANO_NWHITE = 14;  // white keys in [PIANO_LO, PIANO_HI]
static const int PIANO_X    = 20;
static const int PIANO_Y    = 656;
static const int PIANO_H    = 54;    // white key height
static const int PIANO_WKW  = (CX - 40) / PIANO_NWHITE;  // white key width
static const int PIANO_BKW  = PIANO_WKW * 3 / 5;         // black key width
static const int PIANO_BKH  = PIANO_H  * 3 / 5;          // black key height

static const int HELP_Y     = 722;

// ── pitch history ─────────────────────────────────────────────────────────────
static constexpr int   HISTORY_MAX     = 780;  // matches strip width in pixels
static constexpr float SILENCE_SENTINEL = 1e10f;
static float g_history[HISTORY_MAX];
static int   g_history_pos   = 0;   // circular write head
static int   g_history_count = 0;   // valid entries, capped at HISTORY_MAX

static void history_push(float cents_or_sentinel) {
    g_history[g_history_pos % HISTORY_MAX] = cents_or_sentinel;
    g_history_pos++;
    if (g_history_count < HISTORY_MAX) g_history_count++;
}

static void history_clear() {
    g_history_pos   = 0;
    g_history_count = 0;
}

// ── app state ─────────────────────────────────────────────────────────────────
static TonePlayer    g_player;
static MicCapture    g_capture;
static PitchDetector g_detector;

static int              g_target   = 60;
static float            g_smoothed = 0.0f;
static int              g_silent   = 0;
static HarmonicSpectrum g_spectrum = {};
static bool             g_has_spec = false;
static Formants         g_formants = {0.f, 0.f, false};
static short            g_samples[2048];

struct VibratoInfo { bool detected; float rate_hz; float depth_cents; };

static constexpr int HOLD_FRAMES = 20;

// Bounding rect of the target note head — updated each paint, used for click hit-test.
static RECT g_note_hit = {};

// MIDI note under the mouse cursor while hovering the left staff area (-1 = none).
static int g_hover_midi = -1;

// ── practice modes ────────────────────────────────────────────────────────────
enum class Mode { FREE = 0, INTERVAL, SCALE, COUNT };
static Mode g_mode = Mode::FREE;

// ── hold timer ────────────────────────────────────────────────────────────────
// Counts consecutive 50ms ticks where pitch is within ±15 ct of target.
static int g_hold_frames  = 0;
static constexpr int HOLD_TARGET = 40; // 2 seconds to complete the ring

// ── sequenced play (root → interval target) ───────────────────────────────────
static int g_play_queued = -1;   // MIDI note to play after delay
static int g_play_delay  = 0;   // countdown ticks

// ── interval mode ─────────────────────────────────────────────────────────────
static int g_root_midi   = 60;
static int g_interval_st = 7;   // semitones above root

// ── scale mode ────────────────────────────────────────────────────────────────
static const int SCALE_SEMI[][8] = {
    { 0, 2, 4, 5, 7,  9, 11, 12 },   // major
    { 0, 2, 3, 5, 7,  8, 10, 12 },   // natural minor
    { 0, 2, 4, 7, 9, 12,  0,  0 },   // pentatonic major (6 notes)
};
static const int         SCALE_LEN[]  = { 8, 8, 6 };
static const wchar_t*    SCALE_NAME[] = { L"Major", L"Minor", L"Pentatonic" };

static int g_scale_type  = 0;
static int g_scale_root  = 60;
static int g_scale_notes[8];
static int g_scale_count = 0;
static int g_scale_idx   = 0;

// ── fonts ─────────────────────────────────────────────────────────────────────
static HFONT g_fTitle, g_fLabel, g_fBody, g_fClef;

static void create_fonts() {
    g_fTitle = CreateFontW(28, 0, 0, 0, FW_BOLD,   0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fLabel = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fBody  = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fClef  = CreateFontW(190, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI Symbol");
}

static void delete_fonts() {
    DeleteObject(g_fTitle);
    DeleteObject(g_fLabel);
    DeleteObject(g_fBody);
    DeleteObject(g_fClef);
}

// ── pitch smoothing ───────────────────────────────────────────────────────────
static float smooth_pitch(float prev, float next) {
    if (prev <= 0.0f) return next;
    if (std::abs(cents_error(next, prev)) > 200.0f) return next;
    return 0.3f * next + 0.7f * prev;
}

// ── helpers ───────────────────────────────────────────────────────────────────
static std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), n);
    return w;
}

static int sy(int pos)                   { return STAFF_BOT - pos * STEP; }
static int staff_pos(int midi, bool treble) {
    int pc  = midi % 12;
    int oct = midi / 12 - 1;
    return oct * 7 + CHROM_DIAT[pc] - (treble ? 30 : 18);
}

// Inverse of staff_pos: pixel y → nearest natural-note MIDI (no accidentals).
static int y_to_midi(int y, bool treble) {
    static const int DIAT_CHROM[7] = { 0, 2, 4, 5, 7, 9, 11 };
    int pos      = (int)std::round((float)(STAFF_BOT - y) / STEP);
    int abs_diat = pos + (treble ? 30 : 18);
    int diat_oct = abs_diat / 7;
    int diat_pc  = abs_diat % 7;
    if (diat_pc < 0) { diat_pc += 7; diat_oct--; }
    return std::clamp((diat_oct + 1) * 12 + DIAT_CHROM[diat_pc], 36, 84);
}

// True when pt is in the selectable left-staff zone (target column side).
static bool in_staff_select(POINT pt) {
    int divider = (TARGET_X + DETECTED_X) / 2;
    return pt.x >= STAFF_LEFT && pt.x <= divider &&
           pt.y >= STAFF_TOP - 6 * STEP && pt.y <= STAFF_BOT + 6 * STEP;
}

// Forward declarations for helpers used inside draw_ui
static const wchar_t* interval_name(int semi);

// ── drawing ───────────────────────────────────────────────────────────────────

static void draw_ledger_lines(HDC hdc, int x, int pos, COLORREF col) {
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HPEN op  = (HPEN)SelectObject(hdc, pen);
    for (int p = -2; p >= pos; p -= 2) {
        int y = sy(p);
        MoveToEx(hdc, x - HEAD_W - 5, y, nullptr);
        LineTo  (hdc, x + HEAD_W + 5, y);
    }
    for (int p = 10; p <= pos; p += 2) {
        int y = sy(p);
        MoveToEx(hdc, x - HEAD_W - 5, y, nullptr);
        LineTo  (hdc, x + HEAD_W + 5, y);
    }
    SelectObject(hdc, op);
    DeleteObject(pen);
}

// Returns the pixel Y of the drawn note centre — caller may store it.
static int draw_note(HDC hdc, int x, int midi, bool treble, COLORREF col, bool hollow) {
    int pos = staff_pos(midi, treble);
    int y   = sy(pos);
    int pc  = midi % 12;

    draw_ledger_lines(hdc, x, pos, col);

    HBRUSH br = hollow ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(col);
    HPEN   pn = CreatePen(PS_SOLID, 2, col);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
    HPEN   op = (HPEN)SelectObject(hdc, pn);
    Ellipse(hdc, x - HEAD_W, y - HEAD_H, x + HEAD_W, y + HEAD_H);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    if (!hollow) DeleteObject(br);
    DeleteObject(pn);

    HPEN sp = CreatePen(PS_SOLID, 2, col);
    op = (HPEN)SelectObject(hdc, sp);
    if (pos < 4) {
        MoveToEx(hdc, x + HEAD_W - 1, y, nullptr);
        LineTo  (hdc, x + HEAD_W - 1, y - 44);
    } else {
        MoveToEx(hdc, x - HEAD_W + 1, y, nullptr);
        LineTo  (hdc, x - HEAD_W + 1, y + 44);
    }
    SelectObject(hdc, op);
    DeleteObject(sp);

    if (IS_FLAT[pc] || IS_SHARP[pc]) {
        HFONT hf = CreateFontW(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT of = (HFONT)SelectObject(hdc, hf);
        SetTextColor(hdc, col);
        SetBkMode(hdc, TRANSPARENT);
        const wchar_t* glyph = IS_FLAT[pc] ? L"♭" : L"♯";
        TextOutW(hdc, x - HEAD_W - 13, y - 10, glyph, 1);
        SelectObject(hdc, of);
        DeleteObject(hf);
    }

    return y;
}

static void draw_clef(HDC hdc, bool treble) {
    HFONT of = (HFONT)SelectObject(hdc, g_fClef);
    SetTextColor(hdc, RGB(30, 30, 30));
    SetBkMode(hdc, TRANSPARENT);
    if (treble) {
        // G line = 2nd from bottom = STAFF_TOP + 3*LINE_GAP.
        // At 95 pt the G-curl sits ~82 px below the top of the character cell.
        wchar_t clef[] = { 0xD834, 0xDD1E, 0 }; // U+1D11E G clef
        int g_line = STAFF_TOP + 3 * LINE_GAP;
        TextOutW(hdc, STAFF_LEFT, g_line - 130, clef, 2);
    } else {
        // F line = 4th from bottom = 2nd from top = STAFF_TOP + LINE_GAP.
        // The character top sits a few pixels above the F-line.
        wchar_t clef[] = { 0xD834, 0xDD22, 0 }; // U+1D122 F clef
        int f_line = STAFF_TOP + LINE_GAP;
        TextOutW(hdc, STAFF_LEFT, f_line - 88, clef, 2);
    }
    SelectObject(hdc, of);
}

static void draw_cents_bar(HDC hdc, float cents) {
    // cents is octave-corrected pitch-class error (±600 range).
    // Log scale: log2(1+|c|)/log2(601) so fine deviations near zero are amplified.
    const float MAX = 600.0f;
    auto log_t = [MAX](float c) -> float {
        float s = (c >= 0.0f) ? 1.0f : -1.0f;
        return s * std::log2(1.0f + std::abs(c)) / std::log2(1.0f + MAX);
    };

    int x = 20, y = CENTS_Y, w = CX - 40, h = CENTS_H;
    int mid = x + w / 2;

    // Border
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        HPEN op  = (HPEN)SelectObject(hdc, pen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, x, y, x + w, y + h);
        SelectObject(hdc, op); DeleteObject(pen);
    }

    // Filled bar
    float t   = std::clamp(log_t(cents), -1.0f, 1.0f);
    int   end = mid + (int)(t * (w / 2));
    COLORREF col = std::abs(cents) <= 15.0f ? RGB(50, 180, 80)
                 : std::abs(cents) <= 50.0f ? RGB(210, 160, 0)
                 :                            RGB(210, 60, 60);
    {
        HBRUSH br = CreateSolidBrush(col);
        HPEN   np = CreatePen(PS_NULL, 0, 0);
        HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
        HPEN   op = (HPEN)SelectObject(hdc, np);
        if (end > mid) Rectangle(hdc, mid + 1, y + 1, end,     y + h - 1);
        else           Rectangle(hdc, end,     y + 1, mid - 1, y + h - 1);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(np);
    }

    // Centre line
    {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(80, 80, 80));
        HPEN op  = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, mid, y, nullptr); LineTo(hdc, mid, y + h);
        SelectObject(hdc, op); DeleteObject(pen);
    }

    // Tick marks at ±15, ±50, ±100 cents
    for (float tc : { 15.0f, 50.0f, 100.0f }) {
        for (int s : { -1, 1 }) {
            int tx = mid + (int)(log_t(tc * s) * (w / 2));
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(185, 185, 200));
            HPEN op  = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, tx, y, nullptr); LineTo(hdc, tx, y + h);
            SelectObject(hdc, op); DeleteObject(pen);
        }
    }

    // Labels below: 0, ±15, ±50, ±100
    HFONT of = (HFONT)SelectObject(hdc, g_fBody);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(150, 150, 155));
    TextOutW(hdc, mid - 3, y + h + 3, L"0", 1);
    for (int s : { -1, 1 }) {
        int tx15  = mid + (int)(log_t(15.0f  * s) * (w / 2));
        int tx50  = mid + (int)(log_t(50.0f  * s) * (w / 2));
        int tx100 = mid + (int)(log_t(100.0f * s) * (w / 2));
        TextOutW(hdc, tx15  - 6,  y + h + 3, s > 0 ? L"+15"  : L"-15",  3);
        TextOutW(hdc, tx50  - 6,  y + h + 3, s > 0 ? L"+50"  : L"-50",  3);
        TextOutW(hdc, tx100 - 10, y + h + 3, s > 0 ? L"+100" : L"-100", 4);
    }
    SelectObject(hdc, of);
}

// ── vibrato detection ─────────────────────────────────────────────────────────
// Autocorrelation over the most recent contiguous run of cents values.
// Sampling rate = 20 Hz (50ms timer); detectable vibrato range ≈ 2–10 Hz.
static VibratoInfo detect_vibrato() {
    constexpr int MAX_ANALYSIS = 100;  // 5 s at 20 Hz — enough for multiple cycles
    float buf[MAX_ANALYSIS];
    int n = 0;
    for (int i = 0; i < g_history_count && n < MAX_ANALYSIS; i++) {
        int   idx = ((g_history_pos - 1 - i) % HISTORY_MAX + HISTORY_MAX) % HISTORY_MAX;
        float v   = g_history[idx];
        if (v > 9e8f) break;  // silence: only use the most recent unbroken run
        buf[n++] = v;
    }
    if (n < 30) return {false, 0.0f, 0.0f};  // need ≥1.5 s of continuous pitch

    float mean = 0.0f;
    for (int i = 0; i < n; i++) mean += buf[i];
    mean /= n;
    for (int i = 0; i < n; i++) buf[i] -= mean;

    float power = 0.0f;
    for (int i = 0; i < n; i++) power += buf[i] * buf[i];
    power /= n;
    if (power < 25.0f) return {false, 0.0f, 0.0f};  // < 5 cents RMS: pitch too flat

    float best_acf = -1e9f;
    int   best_lag = 2;
    for (int lag = 2; lag <= 10 && lag < n / 2; lag++) {
        float r = 0.0f;
        for (int i = 0; i < n - lag; i++) r += buf[i] * buf[i + lag];
        r /= (n - lag);
        if (r > best_acf) { best_acf = r; best_lag = lag; }
    }
    if (best_acf < 0.4f * power) return {false, 0.0f, 0.0f};  // weak periodicity

    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < n; i++) { mn = std::min(mn, buf[i]); mx = std::max(mx, buf[i]); }
    float depth = mx - mn;
    if (depth < 20.0f) return {false, 0.0f, 0.0f};  // < ±10 cents: too subtle

    return {true, 20.0f / (float)best_lag, depth};
}

// ── pitch history trace ───────────────────────────────────────────────────────
static void draw_trace(HDC hdc) {
    const int TX  = 20;
    const int TW  = 470;  // right ~490; vowel chart occupies 500–800
    const int TH  = TRACE_H;
    const int TY  = TRACE_Y;
    const int MID = TY + TH / 2;           // y of target (0 cents)
    const float RANGE = 300.0f;            // ±300 cents visible (±3 semitones)

    // Background
    RECT tr = { TX, TY, TX + TW, TY + TH };
    HBRUSH bg = CreateSolidBrush(RGB(242, 244, 248));
    FillRect(hdc, &tr, bg);
    DeleteObject(bg);

    // Semitone grid lines at ±1, ±2, ±3 semitones
    for (int s = -3; s <= 3; s++) {
        if (s == 0) continue;
        float ct = s * 100.0f;
        int   gy = MID - (int)(ct / RANGE * TH / 2);
        if (gy <= TY || gy >= TY + TH) continue;
        COLORREF gc = (std::abs(s) == 1) ? RGB(205, 210, 220) : RGB(190, 196, 210);
        HPEN gp = CreatePen(PS_DOT, 1, gc);
        HPEN op = (HPEN)SelectObject(hdc, gp);
        MoveToEx(hdc, TX + 1, gy, nullptr);
        LineTo  (hdc, TX + TW - 1, gy);
        SelectObject(hdc, op); DeleteObject(gp);
    }

    // Target line (green, solid, centre)
    HPEN tp = CreatePen(PS_SOLID, 2, RGB(50, 180, 80));
    HPEN otp = (HPEN)SelectObject(hdc, tp);
    MoveToEx(hdc, TX + 1, MID, nullptr);
    LineTo  (hdc, TX + TW - 1, MID);
    SelectObject(hdc, otp); DeleteObject(tp);

    // Pitch dots — newest = right edge, old = left (scrolls out)
    int n = std::min(g_history_count, TW - 4);
    for (int i = 0; i < n; i++) {
        int idx = ((g_history_pos - 1 - i) % HISTORY_MAX + HISTORY_MAX) % HISTORY_MAX;
        float cents = g_history[idx];
        if (cents > 9e8f) continue;                    // silence gap

        int x = TX + TW - 2 - i;                      // newest = right
        float t = std::clamp(cents / RANGE, -1.0f, 1.0f);
        int y = MID - (int)(t * TH / 2);
        y = std::max(TY + 2, std::min(TY + TH - 3, y));

        COLORREF col = std::abs(cents) <= 15.0f ? RGB(50, 180, 80)
                     : std::abs(cents) <= 100.0f ? RGB(210, 160, 0)
                     :                             RGB(210, 60, 60);
        SetPixel(hdc, x, y,     col);
        SetPixel(hdc, x, y + 1, col);
    }

    // "now" cursor at right edge
    HPEN np = CreatePen(PS_SOLID, 1, RGB(160, 160, 180));
    HPEN onp = (HPEN)SelectObject(hdc, np);
    MoveToEx(hdc, TX + TW - 2, TY + 1, nullptr);
    LineTo  (hdc, TX + TW - 2, TY + TH - 1);
    SelectObject(hdc, onp); DeleteObject(np);

    // Border
    HPEN bp = CreatePen(PS_SOLID, 1, RGB(200, 205, 215));
    HPEN obp = (HPEN)SelectObject(hdc, bp);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, TX, TY, TX + TW, TY + TH);
    SelectObject(hdc, obp); DeleteObject(bp);

    // Labels
    HFONT of = (HFONT)SelectObject(hdc, g_fBody);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(160, 165, 180));
    TextOutW(hdc, TX + 5, TY + 4,       L"Pitch history",  13);
    TextOutW(hdc, TX + TW - 26, TY + 4, L"now",             3);
    // Semitone axis label
    TextOutW(hdc, TX + 5, TY + TH / 2 - 24, L"+1",  2);
    TextOutW(hdc, TX + 5, TY + TH / 2 + 12, L"-1",  2);

    VibratoInfo vibrato = detect_vibrato();
    if (vibrato.detected) {
        static wchar_t vbuf[48];
        int len = swprintf_s(vbuf, L"~ vibrato  %.1f Hz  ±%.0f ct",
                             vibrato.rate_hz, vibrato.depth_cents / 2.0f);
        if (len > 0) {
            SetTextColor(hdc, RGB(130, 100, 180));
            TextOutW(hdc, TX + 5, TY + TH - 20, vbuf, len);
        }
    }

    {
        static wchar_t gbuf[32];
        float db = 20.0f * std::log10f(g_detector.silence());
        int len = swprintf_s(gbuf, L"Gate: %+.0f dB", db);
        if (len > 0) {
            SetTextColor(hdc, RGB(160, 165, 180));
            TextOutW(hdc, TX + TW - 110, TY + TH - 20, gbuf, len);
        }
    }

    SelectObject(hdc, of);
}

// ── vowel formant chart ───────────────────────────────────────────────────────
static void draw_vowel_chart(HDC hdc) {
    const int VX = 500;
    const int VY = TRACE_Y;
    const int VW = 300;
    const int VH = TRACE_H;

    RECT vr = { VX, VY, VX + VW, VY + VH };
    HBRUSH bg = CreateSolidBrush(RGB(242, 244, 248));
    FillRect(hdc, &vr, bg);
    DeleteObject(bg);

    HPEN bp = CreatePen(PS_SOLID, 1, RGB(200, 205, 215));
    HPEN obp = (HPEN)SelectObject(hdc, bp);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, VX, VY, VX + VW, VY + VH);
    SelectObject(hdc, obp); DeleteObject(bp);

    // Axes: F1 200–1000 Hz vertical (low=top), F2 3000–700 Hz horizontal (high=left)
    const float F1_MIN = 200.f, F1_MAX = 1000.f;
    const float F2_MIN = 700.f, F2_MAX = 3000.f;
    auto vx = [&](float f2) { return VX + (int)(VW * (F2_MAX - f2) / (F2_MAX - F2_MIN)); };
    auto vy = [&](float f1) { return VY + (int)(VH * (f1 - F1_MIN) / (F1_MAX - F1_MIN)); };

    struct Vowel { float f1, f2; const wchar_t* label; int llen; };
    static const Vowel vowels[] = {
        {270.f, 2600.f, L"i",      1},
        {430.f, 2200.f, L"e",      1},
        {600.f, 1800.f, L"ɛ", 1},  // ɛ
        {850.f, 1700.f, L"æ", 1},  // æ
        {800.f, 1100.f, L"ɑ", 1},  // ɑ
        {450.f, 950.f,  L"o",      1},
        {280.f, 850.f,  L"u",      1},
    };

    HFONT of = (HFONT)SelectObject(hdc, g_fBody);
    SetBkMode(hdc, TRANSPARENT);

    for (const auto& v : vowels) {
        int cx = vx(v.f2);
        int cy = vy(v.f1);
        HPEN rp = CreatePen(PS_SOLID, 1, RGB(180, 190, 210));
        HBRUSH rb = CreateSolidBrush(RGB(210, 220, 235));
        SelectObject(hdc, rp); SelectObject(hdc, rb);
        Ellipse(hdc, cx - 4, cy - 4, cx + 4, cy + 4);
        SelectObject(hdc, GetStockObject(NULL_PEN));
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        DeleteObject(rp); DeleteObject(rb);
        SetTextColor(hdc, RGB(120, 130, 160));
        TextOutW(hdc, cx + 5, cy - 7, v.label, v.llen);
    }

    // Live formant dot
    if (g_formants.valid) {
        int fx = std::clamp(vx(g_formants.f2_hz), VX + 2, VX + VW - 3);
        int fy = std::clamp(vy(g_formants.f1_hz), VY + 2, VY + VH - 3);
        HPEN lp = CreatePen(PS_SOLID, 1, RGB(40, 110, 200));
        HBRUSH lb = CreateSolidBrush(RGB(70, 150, 240));
        SelectObject(hdc, lp); SelectObject(hdc, lb);
        Ellipse(hdc, fx - 6, fy - 6, fx + 6, fy + 6);
        SelectObject(hdc, GetStockObject(NULL_PEN));
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        DeleteObject(lp); DeleteObject(lb);
    }

    // Title + axis hints
    SetTextColor(hdc, RGB(160, 165, 180));
    TextOutW(hdc, VX + 5,        VY + 4,       L"Vowel space", 11);
    TextOutW(hdc, VX + 5,        VY + VH - 18, L"F2→",    3);
    TextOutW(hdc, VX + VW - 22,  VY + VH - 18, L"F1↓",    3);

    SelectObject(hdc, of);
}

// ── piano keyboard strip ──────────────────────────────────────────────────────
static int piano_white_idx(int midi) {
    if (midi < PIANO_LO || midi > PIANO_HI) return -1;
    int pc = midi % 12;
    if (PC_TO_WHITE[pc] < 0) return -1;
    return (midi - PIANO_LO) / 12 * 7 + PC_TO_WHITE[pc];
}

static int piano_black_lx(int midi) {
    if (midi < PIANO_LO || midi > PIANO_HI) return -1;
    int pc = midi % 12;
    if (PC_BLACK_LEFT[pc] < 0) return -1;
    int left_w = (midi - PIANO_LO) / 12 * 7 + PC_BLACK_LEFT[pc];
    return PIANO_X + (left_w + 1) * PIANO_WKW - PIANO_BKW / 2;
}

static int piano_hit_test(POINT pt) {
    if (pt.y < PIANO_Y || pt.y >= PIANO_Y + PIANO_H) return -1;
    if (pt.x < PIANO_X || pt.x >= PIANO_X + PIANO_NWHITE * PIANO_WKW) return -1;
    if (pt.y < PIANO_Y + PIANO_BKH) {  // upper zone: black keys take priority
        for (int m = PIANO_LO; m <= PIANO_HI; m++) {
            int bx = piano_black_lx(m);
            if (bx >= 0 && pt.x >= bx && pt.x < bx + PIANO_BKW) return m;
        }
    }
    int wi = (pt.x - PIANO_X) / PIANO_WKW;
    for (int m = PIANO_LO; m <= PIANO_HI; m++)
        if (piano_white_idx(m) == wi) return m;
    return -1;
}

static void draw_piano_strip(HDC hdc) {
    // White keys
    for (int midi = PIANO_LO; midi <= PIANO_HI; midi++) {
        int wi = piano_white_idx(midi);
        if (wi < 0) continue;
        int kx = PIANO_X + wi * PIANO_WKW;

        bool is_target = (midi == g_target);
        bool in_scale  = false;
        if (g_mode == Mode::SCALE)
            for (int i = 0; i < g_scale_count; i++)
                if (g_scale_notes[i] == midi) { in_scale = true; break; }

        COLORREF fill = is_target ? RGB(80, 160, 220)
                      : in_scale  ? RGB(195, 218, 245)
                      :             RGB(252, 252, 254);
        HBRUSH br = CreateSolidBrush(fill);
        HPEN   pn = CreatePen(PS_SOLID, 1, RGB(160, 162, 172));
        HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
        HPEN   op = (HPEN)SelectObject(hdc, pn);
        Rectangle(hdc, kx, PIANO_Y, kx + PIANO_WKW, PIANO_Y + PIANO_H);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(pn);

        if (midi % 12 == 0) {  // octave label on C keys
            HFONT of = (HFONT)SelectObject(hdc, g_fBody);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, is_target ? RGB(255,255,255) : RGB(160, 160, 170));
            wchar_t lbl[4];
            swprintf_s(lbl, L"C%d", midi / 12 - 1);
            RECT lr = { kx, PIANO_Y + PIANO_H - 18, kx + PIANO_WKW, PIANO_Y + PIANO_H };
            DrawTextW(hdc, lbl, -1, &lr, DT_CENTER | DT_SINGLELINE);
            SelectObject(hdc, of);
        }
    }

    // Black keys drawn on top
    for (int midi = PIANO_LO; midi <= PIANO_HI; midi++) {
        int bx = piano_black_lx(midi);
        if (bx < 0) continue;

        bool is_target = (midi == g_target);
        bool in_scale  = false;
        if (g_mode == Mode::SCALE)
            for (int i = 0; i < g_scale_count; i++)
                if (g_scale_notes[i] == midi) { in_scale = true; break; }

        COLORREF fill = is_target ? RGB(50, 120, 200)
                      : in_scale  ? RGB(55, 85, 145)
                      :             RGB(28, 28, 32);
        HBRUSH br = CreateSolidBrush(fill);
        HPEN   np = CreatePen(PS_NULL, 0, 0);
        HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
        HPEN   op = (HPEN)SelectObject(hdc, np);
        Rectangle(hdc, bx, PIANO_Y, bx + PIANO_BKW, PIANO_Y + PIANO_BKH);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(np);
    }
}

// ── harmonic bars ─────────────────────────────────────────────────────────────
static void draw_harmonic_bars(HDC hdc) {
    const int BAR_W   = 48;
    const int BAR_GAP = 18;
    int total_w = 5 * BAR_W + 4 * BAR_GAP;
    int bx = (CX - total_w) / 2;

    HFONT of = (HFONT)SelectObject(hdc, g_fBody);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(130, 130, 140));
    TextOutW(hdc, bx - 88, HARM_TOP + HARM_H / 2 - 7, L"Harmonics", 9);
    SelectObject(hdc, of);

    for (int h = 0; h < 5; h++) {
        int x   = bx + h * (BAR_W + BAR_GAP);
        int bar = (int)(g_spectrum.power[h] * HARM_H);

        HPEN tp = CreatePen(PS_SOLID, 1, RGB(210, 210, 218));
        HPEN otp = (HPEN)SelectObject(hdc, tp);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, x, HARM_TOP, x + BAR_W, HARM_TOP + HARM_H);
        SelectObject(hdc, otp); DeleteObject(tp);

        if (bar > 0) {
            COLORREF col = (h == 0) ? RGB(55, 130, 210) : RGB(130, 180, 230);
            HBRUSH  fb  = CreateSolidBrush(col);
            HPEN    np  = CreatePen(PS_NULL, 0, 0);
            HBRUSH  ofb = (HBRUSH)SelectObject(hdc, fb);
            HPEN    onp = (HPEN)SelectObject(hdc, np);
            Rectangle(hdc, x + 1, HARM_TOP + HARM_H - bar,
                      x + BAR_W - 1, HARM_TOP + HARM_H);
            SelectObject(hdc, ofb); SelectObject(hdc, onp);
            DeleteObject(fb); DeleteObject(np);
        }

        wchar_t lbl[4];
        if (h == 0) lbl[0] = L'F', lbl[1] = 0;
        else        swprintf_s(lbl, L"%dF", h + 1);
        HFONT of2 = (HFONT)SelectObject(hdc, g_fBody);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(110, 110, 120));
        RECT lr = { x, HARM_TOP + HARM_H + 4, x + BAR_W, HARM_TOP + HARM_H + 20 };
        DrawTextW(hdc, lbl, -1, &lr, DT_CENTER | DT_SINGLELINE);
        SelectObject(hdc, of2);
    }
}

// ── play-hint triangle drawn beside the target note head ─────────────────────
static void draw_play_hint(HDC hdc, int x, int y) {
    COLORREF col = RGB(100, 160, 215);
    HBRUSH br = CreateSolidBrush(col);
    HPEN   np = CreatePen(PS_NULL, 0, 0);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
    HPEN   op = (HPEN)SelectObject(hdc, np);
    POINT  pts[] = { {x, y - 6}, {x, y + 6}, {x + 10, y} };
    Polygon(hdc, pts, 3);
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(br); DeleteObject(np);
}

// ── hold timer ring ───────────────────────────────────────────────────────────
static void draw_hold_timer(HDC hdc, int cx, int cy) {
    const int R = 11;
    float frac  = std::min(1.0f, (float)g_hold_frames / HOLD_TARGET);

    // Background circle
    HPEN   gp = CreatePen(PS_SOLID, 2, RGB(210, 212, 218));
    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN   op = (HPEN)SelectObject(hdc, gp);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, nb);
    Ellipse(hdc, cx - R, cy - R, cx + R, cy + R);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(gp);

    if (frac >= 1.0f) {
        // Complete: filled green circle
        HBRUSH fb = CreateSolidBrush(RGB(50, 200, 80));
        HPEN   np = CreatePen(PS_NULL, 0, 0);
        ob = (HBRUSH)SelectObject(hdc, fb);
        op = (HPEN)SelectObject(hdc, np);
        Ellipse(hdc, cx - R + 2, cy - R + 2, cx + R - 2, cy + R - 2);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(fb); DeleteObject(np);
    } else if (frac > 0.01f) {
        // Partial clockwise fill from 12 o'clock using GDI Pie
        // GDI Arc/Pie goes CCW in MM_TEXT; to fill CW from top, swap endpoints.
        float angle = 2.0f * 3.14159265f * frac;
        int   ex    = cx + (int)((R - 2) * std::sinf(angle));
        int   ey    = cy - (int)((R - 2) * std::cosf(angle));
        HBRUSH fb = CreateSolidBrush(RGB(60, 160, 220));
        HPEN   np = CreatePen(PS_NULL, 0, 0);
        ob = (HBRUSH)SelectObject(hdc, fb);
        op = (HPEN)SelectObject(hdc, np);
        Pie(hdc, cx - R + 2, cy - R + 2, cx + R - 2, cy + R - 2,
            ex, ey, cx, cy - R + 2);   // arc CCW from (ex,ey) to top = CW fill
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(fb); DeleteObject(np);
    }
}

// ── scale context: mini note heads for non-current scale notes ────────────────
static void draw_scale_context(HDC hdc, bool treble) {
    for (int i = 0; i < g_scale_count; i++) {
        if (i == g_scale_idx) continue;
        int pos = staff_pos(g_scale_notes[i], treble);
        int y   = sy(pos);
        HBRUSH br = CreateSolidBrush(RGB(185, 200, 225));
        HPEN   np = CreatePen(PS_NULL, 0, 0);
        HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
        HPEN   op = (HPEN)SelectObject(hdc, np);
        Ellipse(hdc, TARGET_X - 7, y - 5, TARGET_X + 7, y + 5);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(np);
    }
}

// ── main draw ─────────────────────────────────────────────────────────────────
static void draw_ui(HDC hdc) {
    RECT rc = { 0, 0, CX, CY };
    HBRUSH bg = CreateSolidBrush(RGB(248, 248, 250));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    bool treble = (g_target >= 60);

    // ── Title ────────────────────────────────────────────────────────────────
    HFONT of = (HFONT)SelectObject(hdc, g_fTitle);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(35, 35, 40));
    TextOutW(hdc, 20, 14, L"penala-tuna", 11);

    // Mode badge (top-right)
    {
        static const wchar_t* BADGE[] = { L"FREE", L"INTERVAL", L"SCALE" };
        SelectObject(hdc, g_fLabel);
        COLORREF mc = (g_mode == Mode::FREE) ? RGB(170, 170, 180) : RGB(50, 120, 200);
        SetTextColor(hdc, mc);
        const wchar_t* bn = BADGE[(int)g_mode];
        TextOutW(hdc, CX - 110, 20, bn, (int)wcslen(bn));
    }
    if (g_player.is_playing()) {
        SelectObject(hdc, g_fLabel);
        SetTextColor(hdc, RGB(0, 130, 180));
        TextOutW(hdc, CX - 230, 20, L"[ playing ]", 11);
    }

    HPEN sp = CreatePen(PS_SOLID, 1, RGB(210, 210, 215));
    HPEN osp = (HPEN)SelectObject(hdc, sp);
    MoveToEx(hdc, 0, 56, nullptr); LineTo(hdc, CX, 56);
    SelectObject(hdc, osp); DeleteObject(sp);

    // ── Column labels ────────────────────────────────────────────────────────
    SelectObject(hdc, g_fBody);
    SetTextColor(hdc, RGB(160, 160, 170));
    {
        RECT r = { TARGET_X - 50, 68, TARGET_X + 50, 86 };
        DrawTextW(hdc, L"TARGET", -1, &r, DT_CENTER | DT_SINGLELINE);
    }
    {
        COLORREF dc = (g_smoothed > 0.0f) ? RGB(130, 130, 140) : RGB(195, 195, 205);
        SetTextColor(hdc, dc);
        RECT r = { DETECTED_X - 60, 68, DETECTED_X + 60, 86 };
        DrawTextW(hdc, L"DETECTED", -1, &r, DT_CENTER | DT_SINGLELINE);
    }

    // ── Staff ────────────────────────────────────────────────────────────────
    HPEN lp = CreatePen(PS_SOLID, 1, RGB(40, 40, 44));
    HPEN olp = (HPEN)SelectObject(hdc, lp);
    for (int i = 0; i < 5; i++) {
        int y = STAFF_TOP + i * LINE_GAP;
        MoveToEx(hdc, STAFF_LEFT, y, nullptr);
        LineTo  (hdc, STAFF_RIGHT, y);
    }
    SelectObject(hdc, olp); DeleteObject(lp);

    draw_clef(hdc, treble);

    // Scale mode: mini note heads for all non-current scale notes
    if (g_mode == Mode::SCALE) draw_scale_context(hdc, treble);

    // Ghost note while hovering the staff to select a new target (FREE only)
    if (g_mode == Mode::FREE && g_hover_midi >= 0 && g_hover_midi != g_target)
        draw_note(hdc, TARGET_X, g_hover_midi, treble, RGB(170, 195, 225), true);

    // Target note (hollow) + play-hint triangle + hit rect update
    {
        int note_y = draw_note(hdc, TARGET_X, g_target, treble, RGB(30, 30, 35), true);
        draw_play_hint(hdc, TARGET_X + HEAD_W + 6, note_y);

        // Generous hit rect so the click area includes stem + triangle hint
        int pad = 22;
        g_note_hit = { TARGET_X - pad, note_y - pad,
                       TARGET_X + HEAD_W + 18, note_y + pad };
    }

    // Detected note (filled, coloured) + octave-error shadow
    if (g_smoothed > 0.0f) {
        int   dm       = freq_to_midi(g_smoothed);
        float cents    = cents_error(g_smoothed, midi_to_freq(g_target));
        int   midi_diff = dm - g_target;

        // If ≥1 octave off, draw a hollow shadow in the nearest correct octave
        if (std::abs(midi_diff) >= 12) {
            int octaves    = (int)std::round((float)midi_diff / 12.0f);
            int shadow_midi = dm - octaves * 12;
            draw_note(hdc, DETECTED_X, shadow_midi, treble, RGB(155, 155, 205), true);
        }

        COLORREF col = std::abs(cents) <= 15.0f ? RGB(50, 180, 80)
                     : std::abs(cents) <= 50.0f ? RGB(210, 160, 0)
                     :                            RGB(210, 60, 60);
        draw_note(hdc, DETECTED_X, dm, treble, col, false);
    }

    // Faint divider between note columns
    {
        HPEN gp = CreatePen(PS_DOT, 1, RGB(220, 220, 226));
        HPEN ogp = (HPEN)SelectObject(hdc, gp);
        int mx = (TARGET_X + DETECTED_X) / 2;
        MoveToEx(hdc, mx, STAFF_TOP - 30, nullptr);
        LineTo  (hdc, mx, STAFF_BOT  + 30);
        SelectObject(hdc, ogp); DeleteObject(gp);
    }

    // ── Info text ────────────────────────────────────────────────────────────
    SelectObject(hdc, g_fLabel);

    {
        wchar_t buf[64];
        swprintf_s(buf, L"Target:    %s   %.0f Hz",
                   widen(note_name(g_target)).c_str(), midi_to_freq(g_target));
        SetTextColor(hdc, RGB(50, 50, 56));
        TextOutW(hdc, 20, INFO_Y1, buf, (int)wcslen(buf));
    }

    if (g_smoothed > 0.0f) {
        int   dm    = freq_to_midi(g_smoothed);
        float cents = cents_error(g_smoothed, midi_to_freq(g_target));
        bool  locked = std::abs(cents) <= 15.0f;
        COLORREF col = locked                    ? RGB(50, 180, 80)
                     : std::abs(cents) <= 50.0f ? RGB(210, 160, 0)
                     :                            RGB(210, 60, 60);

        wchar_t buf[80];
        swprintf_s(buf, L"Detected:  %s   %.0f Hz   %+.0f ct",
                   widen(note_name(dm)).c_str(), g_smoothed, cents);
        SetTextColor(hdc, col);
        TextOutW(hdc, 20, INFO_Y2, buf, (int)wcslen(buf));

        if (locked) {
            SelectObject(hdc, g_fTitle);
            SetTextColor(hdc, RGB(50, 180, 80));
            TextOutW(hdc, CX - 120, INFO_Y2 - 2, L"LOCKED", 6);
        }

        // Hold timer ring (always shown when pitch detected)
        draw_hold_timer(hdc, CX - 48, INFO_Y2 + 10);

        // ── INFO_Y3: mode-specific context row ────────────────────────────────
        SelectObject(hdc, g_fLabel);
        SetBkMode(hdc, TRANSPARENT);
        if (g_mode == Mode::INTERVAL) {
            wchar_t buf2[100];
            swprintf_s(buf2, L"Interval:  %s    root: %s",
                       interval_name(g_interval_st),
                       widen(note_name(g_root_midi)).c_str());
            SetTextColor(hdc, RGB(70, 100, 170));
            TextOutW(hdc, 20, INFO_Y3, buf2, (int)wcslen(buf2));
        } else if (g_mode == Mode::SCALE) {
            wchar_t buf2[100];
            swprintf_s(buf2, L"Scale:  %s  root: %s    note %d / %d    hold to advance",
                       SCALE_NAME[g_scale_type],
                       widen(note_name(g_scale_root)).c_str(),
                       g_scale_idx + 1, g_scale_count);
            SetTextColor(hdc, RGB(70, 100, 170));
            TextOutW(hdc, 20, INFO_Y3, buf2, (int)wcslen(buf2));
        }

        // Fold raw cents into ±600 (nearest pitch class) for the bar
        float cents_pc = cents - std::roundf(cents / 1200.0f) * 1200.0f;
        draw_cents_bar(hdc, cents_pc);
        if (g_has_spec) draw_harmonic_bars(hdc);
        draw_piano_strip(hdc);

    } else {
        SelectObject(hdc, g_fLabel);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(170, 170, 180));
        TextOutW(hdc, 20, INFO_Y2, L"Listening…", 10);

        // Mode hint even while silent
        if (g_mode == Mode::INTERVAL) {
            wchar_t buf2[100];
            swprintf_s(buf2, L"Interval:  %s    root: %s",
                       interval_name(g_interval_st),
                       widen(note_name(g_root_midi)).c_str());
            SetTextColor(hdc, RGB(170, 185, 215));
            TextOutW(hdc, 20, INFO_Y3, buf2, (int)wcslen(buf2));
        } else if (g_mode == Mode::SCALE) {
            wchar_t buf2[100];
            swprintf_s(buf2, L"Scale:  %s  root: %s    note %d / %d",
                       SCALE_NAME[g_scale_type],
                       widen(note_name(g_scale_root)).c_str(),
                       g_scale_idx + 1, g_scale_count);
            SetTextColor(hdc, RGB(170, 185, 215));
            TextOutW(hdc, 20, INFO_Y3, buf2, (int)wcslen(buf2));
        }
    }

    // ── Pitch history trace + vowel chart ────────────────────────────────────
    draw_trace(hdc);
    draw_vowel_chart(hdc);

    // ── Piano strip ───────────────────────────────────────────────────────────
    draw_piano_strip(hdc);

    // ── Help ─────────────────────────────────────────────────────────────────
    SelectObject(hdc, g_fBody);
    SetTextColor(hdc, RGB(165, 165, 175));
    static const wchar_t* help_text[] = {
        L"Tab: mode    Click staff / L/R: set note    Up/Dn: octave    Space: replay    Enter: random    [ / ]: gate    Q: quit",
        L"Tab: mode    Space / Enter: new interval (plays root then target)    Q: quit",
        L"Tab: mode    Enter: new scale    Space: replay    hold in tune 2s to advance    Q: quit",
    };
    TextOutW(hdc, 20, HELP_Y, help_text[(int)g_mode],
             (int)wcslen(help_text[(int)g_mode]));

    SelectObject(hdc, of);
}

// ── change note (centralises all the state resets) ───────────────────────────
static void set_target(int midi) {
    g_target      = midi;
    g_smoothed    = 0.0f;
    g_silent      = 0;
    g_has_spec    = false;
    g_hold_frames = 0;
    history_clear();
    g_player.play(midi_to_freq(g_target));
}

// ── interval helpers ──────────────────────────────────────────────────────────
static const wchar_t* interval_name(int semi) {
    static const wchar_t* N[] = {
        L"Unison", L"Minor 2nd", L"Major 2nd", L"Minor 3rd", L"Major 3rd",
        L"Perfect 4th", L"Tritone", L"Perfect 5th",
        L"Minor 6th", L"Major 6th", L"Minor 7th", L"Major 7th", L"Octave"
    };
    if (semi == 12) return N[12];
    int s = ((semi % 12) + 12) % 12;
    return (s < 13) ? N[s] : L"?";
}

static void start_interval_mode() {
    g_root_midi   = random_note(48, 67);
    g_interval_st = random_note(1, 11);
    g_target      = std::clamp(g_root_midi + g_interval_st, 36, 84);
    g_smoothed = 0.0f; g_silent = 0; g_has_spec = false; g_hold_frames = 0;
    history_clear();
    g_player.play(midi_to_freq(g_root_midi), 0.8f);
    g_play_queued = g_target;
    g_play_delay  = 16; // 800ms gap
}

// ── scale helpers ─────────────────────────────────────────────────────────────
static void build_scale() {
    g_scale_count = 0;
    int len = SCALE_LEN[g_scale_type];
    for (int i = 0; i < len; i++) {
        int m = g_scale_root + SCALE_SEMI[g_scale_type][i];
        if (m >= 36 && m <= 84) g_scale_notes[g_scale_count++] = m;
    }
}

static void start_scale_mode() {
    g_scale_root = random_note(48, 60);
    g_scale_type = random_note(0, 2);
    build_scale();
    g_scale_idx  = 0;
    g_smoothed = 0.0f; g_silent = 0; g_has_spec = false; g_hold_frames = 0;
    history_clear();
    if (g_scale_count > 0) {
        g_target = g_scale_notes[0];
        g_player.play(midi_to_freq(g_target));
    }
}

// ── WndProc ───────────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE:
        create_fonts();
        g_capture.start();
        set_target(random_note());
        SetTimer(hwnd, 1, 50, nullptr);
        return 0;

    case WM_TIMER: {
        if (g_capture.read_latest(g_samples, 2048) == 2048) {
            float raw = g_detector.detect(g_samples, 2048);
            if (raw > 0.0f) {
                g_smoothed = smooth_pitch(g_smoothed, raw);
                g_silent   = 0;
                g_spectrum = g_detector.harmonics(g_samples, 2048, g_smoothed);
                g_formants = g_detector.formants(g_samples, 2048);
                g_has_spec = true;
                history_push(cents_error(g_smoothed, midi_to_freq(g_target)));
            } else {
                if (++g_silent > HOLD_FRAMES) {
                    g_smoothed     = 0.0f;
                    g_has_spec     = false;
                    g_formants.valid = false;
                }
                history_push(SILENCE_SENTINEL);
            }
        }

        // Hold timer: count consecutive ticks within ±15ct (octave-corrected)
        if (g_smoothed > 0.0f) {
            float raw_ct = cents_error(g_smoothed, midi_to_freq(g_target));
            float ct_pc  = raw_ct - std::roundf(raw_ct / 1200.0f) * 1200.0f;
            if (std::abs(ct_pc) <= 15.0f) {
                g_hold_frames = std::min(g_hold_frames + 1, HOLD_TARGET);
                // Scale mode: advance when held long enough
                if (g_mode == Mode::SCALE && g_hold_frames >= HOLD_TARGET) {
                    g_scale_idx = (g_scale_idx + 1) % g_scale_count;
                    set_target(g_scale_notes[g_scale_idx]);
                }
            } else {
                g_hold_frames = 0;
            }
        } else if (g_silent == 0) {
            g_hold_frames = 0; // sound present but below threshold → reset
        }
        // While silently held (g_silent in 1..HOLD_FRAMES), keep g_hold_frames frozen.

        // Queued play: root → target sequence for interval mode
        --g_play_delay;
        if (g_play_queued >= 0 && g_play_delay <= 0) {
            g_player.play(midi_to_freq(g_play_queued), 1.2f);
            g_play_queued = -1;
        }

        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC     mdc  = CreateCompatibleDC(hdc);
        HBITMAP mbmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP obmp = (HBITMAP)SelectObject(mdc, mbmp);
        draw_ui(mdc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, obmp);
        DeleteObject(mbmp);
        DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        int prev = g_hover_midi;
        g_hover_midi = in_staff_select(pt) ? y_to_midi(pt.y, g_target >= 60) : -1;
        if (g_hover_midi != prev) InvalidateRect(hwnd, nullptr, FALSE);
        // Request WM_MOUSELEAVE so we can clear the ghost when cursor exits
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hover_midi >= 0) { g_hover_midi = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        if (g_mode == Mode::FREE) {
            int piano_midi = piano_hit_test(pt);
            if (piano_midi >= 0) { set_target(piano_midi); return 0; }
        }
        if (in_staff_select(pt) && g_mode == Mode::FREE)
            set_target(y_to_midi(pt.y, g_target >= 60));
        else if (PtInRect(&g_note_hit, pt))
            g_player.play(midi_to_freq(g_target));
        return 0;
    }

    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            bool over_piano = (g_mode == Mode::FREE && piano_hit_test(pt) >= 0);
            if (in_staff_select(pt) || PtInRect(&g_note_hit, pt) || over_piano) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }

    case WM_KEYDOWN:
        switch (wp) {
        case VK_TAB:
            g_mode = (Mode)(((int)g_mode + 1) % (int)Mode::COUNT);
            if      (g_mode == Mode::INTERVAL) start_interval_mode();
            else if (g_mode == Mode::SCALE)    start_scale_mode();
            else                               set_target(random_note());
            break;
        case VK_SPACE:
            if (g_mode == Mode::INTERVAL) {
                // Replay root → target sequence
                g_player.play(midi_to_freq(g_root_midi), 0.8f);
                g_play_queued = g_target;
                g_play_delay  = 16;
            } else {
                g_player.play(midi_to_freq(g_target));
            }
            break;
        case VK_RETURN:
            if      (g_mode == Mode::INTERVAL) start_interval_mode();
            else if (g_mode == Mode::SCALE)    start_scale_mode();
            else                               set_target(random_note());
            break;
        case VK_UP:
            if (g_mode == Mode::FREE) set_target(std::min(g_target + 12, 84));
            break;
        case VK_DOWN:
            if (g_mode == Mode::FREE) set_target(std::max(g_target - 12, 36));
            break;
        case VK_RIGHT:
            if (g_mode == Mode::FREE) set_target(std::min(g_target + 1, 84));
            break;
        case VK_LEFT:
            if (g_mode == Mode::FREE) set_target(std::max(g_target - 1, 36));
            break;
        case 'Q': case VK_ESCAPE:
            DestroyWindow(hwnd);
            break;
        case VK_OEM_4: // [  — lower noise gate
            g_detector.set_silence(std::clamp(g_detector.silence() * 0.7f, 0.00005f, 0.005f));
            break;
        case VK_OEM_6: // ]  — raise noise gate
            g_detector.set_silence(std::clamp(g_detector.silence() * 1.4f, 0.00005f, 0.005f));
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_capture.stop();
        delete_fonts();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── WinMain ───────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSW wc     = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"PenalaTuna";
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT  r     = { 0, 0, CX, CY };
    AdjustWindowRect(&r, style, FALSE);

    HWND hwnd = CreateWindowW(L"PenalaTuna", L"penala-tuna",
                               style,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top,
                               nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
