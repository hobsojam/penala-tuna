#pragma once
#include <string>

// MIDI 69 = A4 = 440 Hz, semitone ratio 2^(1/12)
float       midi_to_freq(int midi);
int         freq_to_midi(float freq);     // nearest semitone
std::string note_name(int midi);          // e.g. "B♭4"
float       cents_error(float detected, float target);
int         random_note(int lo = 48, int hi = 72); // C3..C5
