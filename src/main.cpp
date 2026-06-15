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

// ── layout constants ──────────────────────────────────────────────────────────
static const int CX         = 820;
static const int CY         = 702;

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
static const int CENTS_Y    = 360;
static const int CENTS_H    = 14;

static const int TRACE_Y    = 388;   // pitch-history strip
static const int TRACE_H    = 148;

static const int HARM_TOP   = 550;   // harmonic bars
static const int HARM_H     = 65;

static const int HELP_Y     = 674;

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
static short            g_samples[2048];

static constexpr int HOLD_FRAMES = 20;

// Bounding rect of the target note head — updated each paint, used for click hit-test.
static RECT g_note_hit = {};

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

// ── pitch history trace ───────────────────────────────────────────────────────
static void draw_trace(HDC hdc) {
    const int TX  = 20;
    const int TW  = CX - 40;
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
    SelectObject(hdc, of);
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

    if (g_player.is_playing()) {
        SelectObject(hdc, g_fLabel);
        SetTextColor(hdc, RGB(0, 130, 180));
        TextOutW(hdc, CX - 110, 20, L"[ playing ]", 11);
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

        // Fold raw cents into ±600 (nearest pitch class) for the bar
        float cents_pc = cents - std::roundf(cents / 1200.0f) * 1200.0f;
        draw_cents_bar(hdc, cents_pc);
        if (g_has_spec) draw_harmonic_bars(hdc);

    } else {
        SetTextColor(hdc, RGB(170, 170, 180));
        TextOutW(hdc, 20, INFO_Y2, L"Listening…", 10);
    }

    // ── Pitch history trace ───────────────────────────────────────────────────
    draw_trace(hdc);

    // ── Help ─────────────────────────────────────────────────────────────────
    SelectObject(hdc, g_fBody);
    SetTextColor(hdc, RGB(165, 165, 175));
    static const wchar_t help[] =
        L"Space: replay    Up/Down: octave    Enter: next note    Q / Esc: quit";
    TextOutW(hdc, 20, HELP_Y, help, (int)wcslen(help));

    SelectObject(hdc, of);
}

// ── change note (centralises all the state resets) ───────────────────────────
static void set_target(int midi) {
    g_target   = midi;
    g_smoothed = 0.0f;
    g_silent   = 0;
    g_has_spec = false;
    history_clear();
    g_player.play(midi_to_freq(g_target));
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
                g_has_spec = true;
                history_push(cents_error(g_smoothed, midi_to_freq(g_target)));
            } else {
                if (++g_silent > HOLD_FRAMES) {
                    g_smoothed = 0.0f;
                    g_has_spec = false;
                }
                history_push(SILENCE_SENTINEL);
            }
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

    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        if (PtInRect(&g_note_hit, pt))
            g_player.play(midi_to_freq(g_target));
        return 0;
    }

    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&g_note_hit, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }

    case WM_KEYDOWN:
        switch (wp) {
        case VK_SPACE:
            g_player.play(midi_to_freq(g_target));
            break;
        case VK_RETURN:
            set_target(random_note());
            break;
        case VK_UP:
            set_target(std::min(g_target + 12, 84));
            break;
        case VK_DOWN:
            set_target(std::max(g_target - 12, 36));
            break;
        case 'Q': case VK_ESCAPE:
            DestroyWindow(hwnd);
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
