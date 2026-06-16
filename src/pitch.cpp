#include "pitch.h"
#include <cmath>
#include <algorithm>

namespace {
    constexpr int SR        = 44100;
    constexpr int WINDOW    = 2048;
    constexpr int MIN_LAG   = SR / 1200;
    constexpr int MAX_LAG   = SR / 60;
    constexpr int LAG_RANGE = MAX_LAG - MIN_LAG + 1;
}

float PitchDetector::detect(const short* samples, int count) {
    if (count < WINDOW) return 0.0f;

    float x[WINDOW];
    float energy = 0.0f;
    for (int i = 0; i < WINDOW; i++) {
        x[i]    = samples[i] / 32768.0f;
        energy  += x[i] * x[i];
    }
    if (energy / WINDOW < silence_) return 0.0f;

    // Autocorrelation over the vocal lag range
    float acf[LAG_RANGE];
    for (int li = 0; li < LAG_RANGE; li++) {
        int   lag = MIN_LAG + li;
        float r   = 0.0f;
        for (int i = 0; i < WINDOW - lag; i++)
            r += x[i] * x[i + lag];
        acf[li] = r;
    }

    // Peak search
    int best = 0;
    for (int li = 1; li < LAG_RANGE; li++)
        if (acf[li] > acf[best]) best = li;

    // Octave-error guard: if half the lag has ≥85% of the peak's energy,
    // the true fundamental is an octave up (we grabbed a subharmonic).
    int half_lag = (MIN_LAG + best) / 2 - MIN_LAG;
    if (half_lag >= 0 && half_lag < LAG_RANGE && acf[half_lag] >= 0.85f * acf[best])
        best = half_lag;

    // Parabolic interpolation for sub-sample accuracy
    float refined = static_cast<float>(MIN_LAG + best);
    if (best > 0 && best < LAG_RANGE - 1) {
        float y0 = acf[best - 1], y1 = acf[best], y2 = acf[best + 1];
        float d  = 2.0f * y1 - y0 - y2;
        if (d > 1e-6f) refined += 0.5f * (y2 - y0) / d;
    }

    return static_cast<float>(SR) / refined;
}

Formants PitchDetector::formants(const short* samples, int count) {
    if (count < WINDOW) return {0.f, 0.f, false};

    // Pre-emphasis + windowing into float buffer
    constexpr int P = 10;
    float x[WINDOW];
    float energy = 0.0f;
    x[0] = samples[0] / 32768.0f;
    for (int i = 1; i < WINDOW; i++) {
        float w = 0.5f - 0.5f * std::cosf(2.0f * 3.14159265f * i / (WINDOW - 1));
        x[i] = w * (samples[i] / 32768.0f - 0.97f * samples[i - 1] / 32768.0f);
        energy += x[i] * x[i];
    }
    if (energy / WINDOW < silence_) return {0.f, 0.f, false};

    // Autocorrelation lags 0..P
    float r[P + 1] = {};
    for (int lag = 0; lag <= P; lag++)
        for (int i = 0; i < WINDOW - lag; i++)
            r[lag] += x[i] * x[i + lag];

    // Levinson-Durbin recursion for LPC coefficients a[1..P]
    float a[P + 1] = {};
    float tmp[P + 1] = {};
    float err = r[0];
    for (int i = 1; i <= P; i++) {
        float lambda = 0.0f;
        for (int j = 1; j < i; j++)
            lambda += a[j] * r[i - j];
        float k = -(r[i] + lambda) / err;
        a[i] = k;
        for (int j = 1; j < i; j++)
            tmp[j] = a[j] + k * a[i - j];
        for (int j = 1; j < i; j++)
            a[j] = tmp[j];
        err *= (1.0f - k * k);
        if (err <= 0.0f) return {0.f, 0.f, false};
    }

    // Evaluate LPC spectrum magnitude on a fine frequency grid using phasor recursion.
    // The LPC error filter A(z) = 1 + a1*z^-1 + ... + aP*z^-P; vocal tract is 1/A(z).
    // We want peaks in |H(f)| = 1/|A(e^j2πf/SR)|.
    constexpr int GRID = 15;  // Hz per bin
    constexpr int NBINS = SR / 2 / GRID;

    float f1 = 0.f, f2 = 0.f;
    float p1 = 0.f, p2 = 0.f;

    // F1 search range: 250–900 Hz; F2 search range: 900–2800 Hz
    constexpr int F1_LO = 250 / GRID, F1_HI = 900 / GRID;
    constexpr int F2_LO = 900 / GRID, F2_HI = 2800 / GRID;

    // Previous two magnitude values for simple peak detection
    float prev2 = 0.f, prev1 = 0.f;
    float prev2_f = 0.f, prev1_f = 0.f;

    for (int bi = 1; bi < NBINS; bi++) {
        float freq = bi * GRID;
        float step = 2.0f * 3.14159265f * freq / SR;
        float cos_s = std::cosf(step), sin_s = std::sinf(step);

        // Evaluate A(e^jw) via phasor recursion
        float cr = 1.0f, ci = 0.0f;
        float re = 1.0f, im = 0.0f;
        for (int k = 1; k <= P; k++) {
            float ncr = cr * cos_s - ci * sin_s;
            ci = cr * sin_s + ci * cos_s;
            cr = ncr;
            re += a[k] * cr;
            im += a[k] * ci;
        }
        float mag = 1.0f / (re * re + im * im + 1e-12f);

        // Peak detection: prev1 > prev2 && prev1 > cur
        if (prev1 > prev2 && prev1 > mag) {
            if (bi - 1 >= F1_LO && bi - 1 <= F1_HI && prev1 > p1) {
                p1 = prev1; f1 = prev1_f;
            } else if (bi - 1 >= F2_LO && bi - 1 <= F2_HI && prev1 > p2) {
                p2 = prev1; f2 = prev1_f;
            }
        }
        prev2 = prev1; prev1 = mag;
        prev2_f = prev1_f; prev1_f = freq;
    }

    if (f1 < 1.f || f2 < 1.f) return {0.f, 0.f, false};
    return {f1, f2, true};
}

HarmonicSpectrum PitchDetector::harmonics(const short* samples, int count, float fundamental) {
    HarmonicSpectrum result = {};
    if (count < WINDOW || fundamental <= 0.0f) return result;

    float x[WINDOW];
    for (int i = 0; i < WINDOW; i++)
        x[i] = samples[i] / 32768.0f;

    float max_p = 0.0f;
    for (int h = 0; h < 5; h++) {
        float freq = fundamental * (h + 1);
        if (freq >= SR * 0.5f) break; // above Nyquist

        // DFT at exactly `freq` using phasor recursion (avoids per-sample trig calls)
        float step  = 2.0f * 3.14159265f * freq / SR;
        float cos_s = std::cosf(step), sin_s = std::sinf(step);
        float cr = 1.0f, ci = 0.0f;
        float re = 0.0f, im = 0.0f;
        for (int i = 0; i < WINDOW; i++) {
            re += x[i] * cr;
            im += x[i] * ci;
            float ncr = cr * cos_s - ci * sin_s;
            ci = cr * sin_s + ci * cos_s;
            cr = ncr;
        }
        result.power[h] = (re * re + im * im) / (WINDOW * WINDOW / 4.0f);
        if (result.power[h] > max_p) max_p = result.power[h];
    }

    if (max_p > 0.0f)
        for (int h = 0; h < 5; h++)
            result.power[h] /= max_p;

    return result;
}
