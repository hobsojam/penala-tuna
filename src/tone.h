#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <vector>

class TonePlayer {
public:
    static constexpr int SAMPLE_RATE = 44100;

    TonePlayer() = default;
    ~TonePlayer() { stop(); }
    // Owns an HWAVEOUT; copying would double-close it.
    TonePlayer(const TonePlayer&)            = delete;
    TonePlayer& operator=(const TonePlayer&) = delete;

    void play(float freq, float dur_sec = 3.0f);
    void stop();
    bool is_playing() const;

private:
    HWAVEOUT           wave_out = nullptr;
    WAVEHDR            hdr      = {};
    std::vector<short> buf;
};
