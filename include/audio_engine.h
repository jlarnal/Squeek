#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <stdint.h>
#include "tone_library.h"

// Abstract audio output interface (piezo now, I2S DAC later)
class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;
    virtual void begin() = 0;
    virtual void setFrequency(uint32_t hz) = 0;
    virtual void setDuty(uint8_t duty) = 0;    // 0-255
    virtual void silence() = 0;
};

// Tone sequencer driven by GPTimer ISR at ~200 Hz
class AudioEngine {
public:
    AudioEngine() = delete;
    static void init(IAudioOutput* output);
    static void play(const ToneSequence* seq);
    static void stop();
    static bool isPlaying();
};

#endif // AUDIO_ENGINE_H
