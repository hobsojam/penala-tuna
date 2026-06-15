# CLAUDE.md — penala-tuna

## Project Overview

Windows C++ vocal tone trainer. Plays a target note, captures mic input, detects pitch via autocorrelation, and reports cents error on a log-scale bar with a Win32 GDI staff display.

## Development Commands

```bat
rem Configure (first time, from project root)
mkdir build && cd build
cmake .. -G "MinGW Makefiles"

rem Build
cmake --build build

rem Run
build\penala-tuna.exe

rem Tests
cd build && ctest --output-on-failure
```

## Stack

- **Language:** C++17
- **Build:** CMake with MinGW-w64 (GCC 16, w64devkit). Build directory: `build/`.
- **Audio I/O:** WinMM (`waveOut` / `waveIn`) — no external audio libs
- **Pitch detection:** Autocorrelation with parabolic interpolation + octave-error guard
- **UI:** Win32 GDI window (double-buffered)

## Architecture

```
src/
  main.cpp          — WinMain, WndProc, all drawing (staff, clef, notes, bars)
  tone.cpp/.h       — sine wave generation, waveOut playback
  capture.cpp/.h    — waveIn mic capture, ring buffer
  pitch.cpp/.h      — autocorrelation pitch detector + harmonic spectrum
  notes.cpp/.h      — frequency ↔ note name / cents math
```

## Key decisions

- **WinMM over WASAPI:** lower barrier, no COM init, ~20-50ms latency is fine for this use case
- **Autocorrelation over FFT:** voice is harmonic; autocorrelation finds the fundamental more reliably without deps
- **No external deps:** everything in Windows SDK; makes build setup trivial

## Coding conventions

- No comments explaining what code does — names should do that
- Add a comment only for non-obvious constraints or workarounds
- Keep each .cpp under ~200 lines; split if it grows past that
- Use `WINAPI` calling convention annotations where required by WinMM callbacks
- Prefer `std::array` / `std::vector` over raw arrays

## Audio constants

- Sample rate: 44100 Hz
- Bit depth: 16-bit signed PCM
- Channels: mono
- Pitch detection window: 2048 samples
- waveIn/waveOut buffer count: 4 double-buffered

## Note math

- A4 = 440 Hz reference
- Semitone ratio: 2^(1/12)
- Cents = 1200 × log2(detected / target)
- In-tune tolerance: ±15 cents (green / LOCKED)

## Build targets

- `penala-tuna` — main WIN32 executable
- `test_notes` — note math unit tests
- `test_pitch` — pitch detector unit tests
