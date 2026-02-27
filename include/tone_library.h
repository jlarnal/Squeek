#ifndef TONE_LIBRARY_H
#define TONE_LIBRARY_H

#include <stdint.h>

class Print;  // forward decl (Arduino)

struct ToneSegment {
    uint16_t freq_start_hz;
    uint16_t freq_end_hz;
    uint8_t  duty_start;      // 0-255: 0=silence, 255=max LEDC duty
    uint8_t  duty_end;
    uint16_t duration_ms;
};

struct ToneSequence {
    const ToneSegment* segments;
    uint8_t count;
    uint8_t repeats;          // 0 = play once, 255 = loop forever
};

class ToneLibrary {
public:
    ToneLibrary() = delete;
    static const ToneSequence* get(const char* name);
    static const ToneSequence* getByIndex(uint8_t index);
    static uint8_t count();
    static const char* nameByIndex(uint8_t index);
    static void list(Print& out);
};

#endif // TONE_LIBRARY_H
