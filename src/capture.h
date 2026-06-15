#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

class MicCapture {
public:
    static constexpr int SAMPLE_RATE  = 44100;
    static constexpr int BUF_SAMPLES  = 2205;          // ~50ms per buffer
    static constexpr int NUM_BUFS     = 4;
    static constexpr int RING_SAMPLES = SAMPLE_RATE * 2; // 2s ring

    ~MicCapture() { stop(); }

    void start();
    void stop();

    // Copies the latest `count` samples into dst.
    // Returns count on success, 0 if not enough data yet.
    int read_latest(short* dst, int count);

private:
    HWAVEIN   wave_in = nullptr;
    HANDLE    event   = nullptr;
    std::thread          thread;
    std::atomic<bool>    running{false};

    std::array<std::vector<short>, NUM_BUFS> bufs;
    std::array<WAVEHDR, NUM_BUFS>            hdrs;

    std::vector<short> ring;
    int                write_pos = 0;
    std::mutex         ring_mtx;

    void capture_loop();
};
