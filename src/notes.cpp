#include "notes.h"
#include <cmath>
#include <random>
#include <string>

// Chromatic scale using flats for accidentals (more idiomatic for voice)
static const char* NAMES[12] = {
    "C", "C♯", "D", "E♭", "E",
    "F", "F♯", "G", "A♭", "A", "B♭", "B"
};

float midi_to_freq(int midi) {
    return 440.0f * std::pow(2.0f, (midi - 69) / 12.0f);
}

int freq_to_midi(float freq) {
    return static_cast<int>(std::round(69.0f + 12.0f * std::log2(freq / 440.0f)));
}

std::string note_name(int midi) {
    int pc     = ((midi % 12) + 12) % 12;
    int octave = midi / 12 - 1;
    return std::string(NAMES[pc]) + std::to_string(octave);
}

float cents_error(float detected, float target) {
    return 1200.0f * std::log2(detected / target);
}

int random_note(int lo, int hi) {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng);
}
