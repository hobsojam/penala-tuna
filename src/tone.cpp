#include "tone.h"
#include <cmath>

void TonePlayer::play(float freq, float dur_sec) {
    stop();

    int n    = static_cast<int>(SAMPLE_RATE * dur_sec);
    int fade = static_cast<int>(SAMPLE_RATE * 0.015f); // 15ms fade in/out
    buf.resize(n);

    for (int i = 0; i < n; i++) {
        float env = 1.0f;
        if (i < fade)        env = (float)i / fade;
        else if (i > n-fade) env = (float)(n - i) / fade;
        buf[i] = static_cast<short>(32767.0f * 0.5f * env
                 * std::sinf(2.0f * 3.14159265f * freq * i / SAMPLE_RATE));
    }

    WAVEFORMATEX fmt = {};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = SAMPLE_RATE;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = sizeof(short);
    fmt.nAvgBytesPerSec = SAMPLE_RATE * sizeof(short);

    if (waveOutOpen(&wave_out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return;

    hdr                = {};
    hdr.lpData         = reinterpret_cast<LPSTR>(buf.data());
    hdr.dwBufferLength = static_cast<DWORD>(n * sizeof(short));

    waveOutPrepareHeader(wave_out, &hdr, sizeof(hdr));
    waveOutWrite(wave_out, &hdr, sizeof(hdr));
}

void TonePlayer::stop() {
    if (!wave_out) return;
    waveOutReset(wave_out);
    waveOutUnprepareHeader(wave_out, &hdr, sizeof(hdr));
    waveOutClose(wave_out);
    wave_out = nullptr;
}

bool TonePlayer::is_playing() const {
    if (!wave_out) return false;
    return !(hdr.dwFlags & WHDR_DONE);
}
