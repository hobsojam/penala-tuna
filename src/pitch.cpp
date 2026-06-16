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
