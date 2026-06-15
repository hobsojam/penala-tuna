#include "notes.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

static void check(const char* label, bool ok) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) std::exit(1);
}

int main() {
    printf("test_notes\n");

    // midi_to_freq: A4 = 440 Hz exactly by definition
    check("A4 = 440 Hz",      std::abs(midi_to_freq(69) - 440.0f) < 0.001f);
    // C4 = 261.626 Hz
    check("C4 ≈ 261.63 Hz",   std::abs(midi_to_freq(60) - 261.626f) < 0.1f);
    // Each octave doubles frequency
    check("A5 = 880 Hz",      std::abs(midi_to_freq(81) - 880.0f) < 0.001f);

    // freq_to_midi round-trips for MIDI 48-84
    bool rt = true;
    for (int m = 48; m <= 84; m++)
        if (freq_to_midi(midi_to_freq(m)) != m) { rt = false; break; }
    check("round-trip midi↔freq for C3..C6", rt);

    // freq_to_midi: 440 Hz → 69
    check("freq_to_midi(440) = 69", freq_to_midi(440.0f) == 69);

    // note_name spot checks (by first character; avoids encoding assumptions)
    check("note_name(60) starts with C", note_name(60)[0] == 'C');
    check("note_name(69) starts with A", note_name(69)[0] == 'A');
    check("note_name(71) starts with B", note_name(71)[0] == 'B');
    // Octave numbers
    check("note_name(60) ends in '4'", note_name(60).back() == '4');
    check("note_name(72) ends in '5'", note_name(72).back() == '5');
    check("note_name(48) ends in '3'", note_name(48).back() == '3');

    // cents_error
    check("unison = 0 ct",       std::abs(cents_error(440.0f, 440.0f)) < 0.001f);
    check("octave up = +1200 ct",std::abs(cents_error(880.0f, 440.0f) - 1200.0f) < 0.01f);
    check("octave down = -1200 ct",std::abs(cents_error(220.0f, 440.0f) + 1200.0f) < 0.01f);
    // Semitone up ≈ +100 cents
    check("semitone up ≈ +100 ct",
          std::abs(cents_error(midi_to_freq(70), midi_to_freq(69)) - 100.0f) < 0.01f);

    printf("all passed\n");
    return 0;
}
