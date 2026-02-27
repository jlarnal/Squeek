#include "tone_library.h"
#include <Arduino.h>
#include <string.h>

// --- Built-in tone segment data ---

static const ToneSegment s_chirpUpSegs[] = {
    { 1000, 4000, 200, 200, 150 }
};

static const ToneSegment s_chirpDownSegs[] = {
    { 4000, 1000, 200, 200, 150 }
};

static const ToneSegment s_squeakSegs[] = {
    { 2000, 4000, 220, 220,  80 },
    { 4000, 2000, 220, 220,  80 }
};

static const ToneSegment s_warbleSegs[] = {
    { 3000, 3000, 200, 200, 60 },
    { 1500, 1500, 200, 200, 60 },
    { 3000, 3000, 200, 200, 60 },
    { 1500, 1500, 200, 200, 60 }
};

static const ToneSegment s_alertSegs[] = {
    { 2500, 2500, 200, 200, 250 },
    {    0,    0,   0,   0, 150 },  // silence
    { 2500, 2500, 200, 200, 250 }
};

// Descending staircase of chirps — each shorter and lower, "fading away"
static const ToneSegment s_fadeChirpSegs[] = {
    { 2000, 4000, 200, 200, 120 },
    {    0,    0,   0,   0,  40 },
    { 1500, 3000, 200, 200,  80 },
    {    0,    0,   0,   0,  40 },
    { 1000, 2000, 200, 200,  50 },
    {    0,    0,   0,   0,  40 },
    {  800, 1200, 200, 200,  30 },
};

// --- Sequence table ---

struct ToneEntry {
    const char* name;
    ToneSequence seq;
};

static const ToneEntry s_tones[] = {
    { "chirp",      { s_chirpUpSegs,   1, 0 } },
    { "chirp_down", { s_chirpDownSegs, 1, 0 } },
    { "squeak",     { s_squeakSegs,    2, 0 } },
    { "warble",     { s_warbleSegs,    4, 0 } },
    { "alert",      { s_alertSegs,     3, 0 } },
    { "fade_chirp", { s_fadeChirpSegs, 7, 0 } },
};
static constexpr int TONE_COUNT = sizeof(s_tones) / sizeof(s_tones[0]);

// --- Public API ---

const ToneSequence* ToneLibrary::getByIndex(uint8_t index) {
    if (index >= TONE_COUNT) return nullptr;
    return &s_tones[index].seq;
}

uint8_t ToneLibrary::count() {
    return TONE_COUNT;
}

const char* ToneLibrary::nameByIndex(uint8_t index) {
    if (index >= TONE_COUNT) return nullptr;
    return s_tones[index].name;
}

const ToneSequence* ToneLibrary::get(const char* name) {
    for (int i = 0; i < TONE_COUNT; i++) {
        if (strcasecmp(name, s_tones[i].name) == 0) {
            return &s_tones[i].seq;
        }
    }
    return nullptr;
}

void ToneLibrary::list(Print& out) {
    out.println("Available tones:");
    for (int i = 0; i < TONE_COUNT; i++) {
        const auto& t = s_tones[i];
        uint16_t total_ms = 0;
        for (uint8_t s = 0; s < t.seq.count; s++) {
            total_ms += t.seq.segments[s].duration_ms;
        }
        out.printf("  %-12s  %u seg(s), %u ms\n", t.name, t.seq.count, total_ms);
    }
}
