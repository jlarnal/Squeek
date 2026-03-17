#include "tone_library.h"
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

// --- Tone groups (each group = one public tone name, N internal variations) ---

struct ToneGroup {
    const char* name;
    const ToneSequence* variations;
    uint8_t count;
};

#include "tone_data.inc"

// --- Public API ---

const ToneSequence* ToneLibrary::getByIndex(uint8_t index) {
    if (index >= GROUP_COUNT) return nullptr;
    const auto& g = s_groups[index];
    return &g.variations[esp_random() % g.count];
}

uint8_t ToneLibrary::count() {
    return GROUP_COUNT;
}

const char* ToneLibrary::nameByIndex(uint8_t index) {
    if (index >= GROUP_COUNT) return nullptr;
    return s_groups[index].name;
}

const ToneSequence* ToneLibrary::get(const char* name) {
    for (int i = 0; i < GROUP_COUNT; i++) {
        if (strcasecmp(name, s_groups[i].name) == 0) {
            const auto& g = s_groups[i];
            return &g.variations[esp_random() % g.count];
        }
    }
    return nullptr;
}

void ToneLibrary::list(Print& out) {
    out.println("Tone groups:");
    for (int i = 0; i < GROUP_COUNT; i++) {
        const auto& g = s_groups[i];
        out.printf("  %-10s  %u variation(s)\n", g.name, g.count);
    }
}
