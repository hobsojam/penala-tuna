#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <vector>

class TonePlayer {
public:
    static constexpr int SAMPLE_RATE = 44100;

    ~TonePlayer() { stop(); }

    void play(float freq, float dur_sec = 3.0f);
    void stop();
    bool is_playing() const;

private:
    HWAVEOUT           wave_out = nullptr;
    WAVEHDR            hdr      = {};
    std::vector<short> buf;
};
