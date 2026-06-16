#pragma once

struct HarmonicSpectrum {
    float power[5]; // normalized power at F, 2F, 3F, 4F, 5F (max=1.0)
};

class PitchDetector {
public:
    // Returns fundamental frequency in Hz, or 0.0f if silent / no pitch.
    float detect(const short* samples, int count);

    // Returns spectral power at the first 5 harmonics of `fundamental`.
    // Call only when detect() returned a valid frequency.
    HarmonicSpectrum harmonics(const short* samples, int count, float fundamental);

    // RMS energy threshold below which detect() treats input as silence.
    void  set_silence(float rms) { silence_ = rms; }
    float silence()        const { return silence_; }

private:
    float silence_ = 0.0005f;
};
