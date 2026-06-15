# penala-tuna

A vocal tone trainer for Windows. Plays a target note, listens to you sing it, and tells you how close you are.

## Features

- Plays a pure sine tone and displays its name and frequency (e.g. B♭ 466 Hz)
- Captures microphone input in real time
- Detects your sung pitch via autocorrelation with parabolic interpolation
- Shows cents error and a live pitch meter
- Locks green when you're within ±15 cents of the target
- Shift the target note up or down an octave on the fly
- Cycles through notes for practice sessions

## Requirements

- Windows 10 or later
- MinGW-w64 (w64devkit or MSYS2) or MSVC (Visual Studio 2022)
- No external libraries — uses WinMM (included with Windows SDK)

## Build

```bat
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
```

With MSVC:

```bat
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

Run tests:

```bat
ctest --output-on-failure
```

## Controls

| Key | Action |
|-----|--------|
| Space | Replay the target tone |
| ↑ / ↓ | Shift target note one octave up / down |
| Enter | Pick a new random note |
| Q | Quit |

## How it works

Sine wave generation fills a PCM buffer at the target frequency and plays it via `waveOut`. Microphone samples are captured via `waveIn` into a 2-second ring buffer. Every 50 ms, autocorrelation runs over a 2048-sample window to find the fundamental frequency; parabolic interpolation gives sub-sample lag accuracy. An octave-error guard checks whether halving the detected lag gives a stronger correlation, catching cases where a subharmonic is mistaken for the fundamental. A snap-on-jump EMA smooths the display without lagging on note changes.

## Name

*Penala* is Indonesian for "tuner." *Tuna* is the thing being tuned — so *penala tuna* is literally "tuna tuner." Also a fish.
