#include "pitch.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr int SR = 44100;

static void check(const char* label, bool ok) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) std::exit(1);
}

static std::vector<short> make_sine(float freq, int n = 4096, float amp = 0.5f) {
    std::vector<short> buf(n);
    for (int i = 0; i < n; i++)
        buf[i] = static_cast<short>(32767.0f * amp
                 * std::sinf(2.0f * 3.14159265f * freq * i / SR));
    return buf;
}

int main() {
    printf("test_pitch\n");
    PitchDetector det;

    // Silence → 0
    std::vector<short> silence(2048, 0);
    check("silence returns 0", det.detect(silence.data(), (int)silence.size()) == 0.0f);

    // Too-short buffer → 0
    std::vector<short> tiny(512, 1000);
    check("short buffer returns 0", det.detect(tiny.data(), (int)tiny.size()) == 0.0f);

    // A4 = 440 Hz: must land within ±5 Hz
    {
        auto buf = make_sine(440.0f);
        float f  = det.detect(buf.data(), (int)buf.size());
        check("A4 440 Hz detected",   f > 0.0f);
        check("A4 within ±5 Hz",      std::abs(f - 440.0f) < 5.0f);
    }

    // C3 ≈ 130.81 Hz
    {
        auto buf = make_sine(130.81f);
        float f  = det.detect(buf.data(), (int)buf.size());
        check("C3 ~130 Hz detected",  f > 0.0f);
        check("C3 within ±5 Hz",      std::abs(f - 130.81f) < 5.0f);
    }

    // E4 ≈ 329.63 Hz
    {
        auto buf = make_sine(329.63f);
        float f  = det.detect(buf.data(), (int)buf.size());
        check("E4 ~329 Hz detected",  f > 0.0f);
        check("E4 within ±5 Hz",      std::abs(f - 329.63f) < 5.0f);
    }

    // Very quiet signal below silence threshold → 0
    {
        auto buf = make_sine(440.0f, 4096, 0.002f); // amplitude = 0.2%
        float f  = det.detect(buf.data(), (int)buf.size());
        check("sub-threshold quiet signal returns 0", f == 0.0f);
    }

    printf("all passed\n");
    return 0;
}
