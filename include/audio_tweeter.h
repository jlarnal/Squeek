#ifndef AUDIO_TWEETER_H
#define AUDIO_TWEETER_H

#include "audio_engine.h"

// LEDC-based push-pull piezo driver on two complementary GPIOs
class PiezoDriver : public IAudioOutput {
public:
    PiezoDriver() = default;
    void begin() override;
    void setFrequency(uint32_t hz) override;
    void setDuty(uint8_t duty) override;    // 0-255 → mapped to LEDC 10-bit
    void silence() override;

    static PiezoDriver& instance();
};

#endif // AUDIO_TWEETER_H
