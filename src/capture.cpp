#include "capture.h"

void MicCapture::start() {
    if (running.load()) return;

    ring.assign(RING_SAMPLES, 0);
    write_pos = 0;

    WAVEFORMATEX fmt = {};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = SAMPLE_RATE;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = sizeof(short);
    fmt.nAvgBytesPerSec = SAMPLE_RATE * sizeof(short);

    event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    if (waveInOpen(&wave_in, WAVE_MAPPER, &fmt,
                   (DWORD_PTR)event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
        return;

    for (int i = 0; i < NUM_BUFS; i++) {
        bufs[i].assign(BUF_SAMPLES, 0);
        hdrs[i]                = {};
        hdrs[i].lpData         = reinterpret_cast<LPSTR>(bufs[i].data());
        hdrs[i].dwBufferLength = BUF_SAMPLES * sizeof(short);
        waveInPrepareHeader(wave_in, &hdrs[i], sizeof(WAVEHDR));
        waveInAddBuffer(wave_in, &hdrs[i], sizeof(WAVEHDR));
    }

    running.store(true);
    waveInStart(wave_in);
    thread = std::thread(&MicCapture::capture_loop, this);
}

void MicCapture::stop() {
    if (!running.load()) return;
    running.store(false);

    waveInStop(wave_in);
    waveInReset(wave_in);
    SetEvent(event); // wake the capture thread so it can exit

    if (thread.joinable()) thread.join();

    for (int i = 0; i < NUM_BUFS; i++)
        waveInUnprepareHeader(wave_in, &hdrs[i], sizeof(WAVEHDR));

    waveInClose(wave_in);
    wave_in = nullptr;
    CloseHandle(event);
    event = nullptr;
}

int MicCapture::read_latest(short* dst, int count) {
    std::lock_guard<std::mutex> lk(ring_mtx);
    if (write_pos < count) return 0;
    int start = write_pos - count;
    for (int i = 0; i < count; i++)
        dst[i] = ring[(start + i) % RING_SAMPLES];
    return count;
}

void MicCapture::capture_loop() {
    while (running.load()) {
        WaitForSingleObject(event, 100);

        for (int i = 0; i < NUM_BUFS; i++) {
            if (!(hdrs[i].dwFlags & WHDR_DONE)) continue;

            int    n   = static_cast<int>(hdrs[i].dwBytesRecorded / sizeof(short));
            auto*  src = reinterpret_cast<short*>(hdrs[i].lpData);

            {
                std::lock_guard<std::mutex> lk(ring_mtx);
                for (int j = 0; j < n; j++)
                    ring[write_pos++ % RING_SAMPLES] = src[j];
            }

            if (!running.load()) break;
            waveInUnprepareHeader(wave_in, &hdrs[i], sizeof(WAVEHDR));
            waveInPrepareHeader(wave_in, &hdrs[i], sizeof(WAVEHDR));
            waveInAddBuffer(wave_in, &hdrs[i], sizeof(WAVEHDR));
        }
    }
}
