#include "audio_tweeter.h"
#include "bsp.hpp"
#include <driver/ledc.h>

// LEDC config: timer 0, channels 0+1, 10-bit resolution (0-1023)
static constexpr ledc_timer_t    LEDC_TIMER   = LEDC_TIMER_0;
static constexpr ledc_mode_t     LEDC_MODE    = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_bit_t LEDC_RES    = LEDC_TIMER_10_BIT;
static constexpr ledc_channel_t  CH_A         = LEDC_CHANNEL_0;
static constexpr ledc_channel_t  CH_B         = LEDC_CHANNEL_1;
static constexpr uint32_t       MAX_DUTY      = (1 << 10) - 1;  // 1023

static bool s_begun = false;

PiezoDriver& PiezoDriver::instance() {
    static PiezoDriver s_instance;
    return s_instance;
}

void PiezoDriver::begin() {
    if (s_begun) return;
    s_begun = true;

    // Configure shared timer
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode      = LEDC_MODE;
    timer_cfg.timer_num       = LEDC_TIMER;
    timer_cfg.duty_resolution = LEDC_RES;
    timer_cfg.freq_hz         = 1000;  // default, will be changed by setFrequency
    timer_cfg.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&timer_cfg);

    // Channel A — positive phase
    ledc_channel_config_t ch_a_cfg = {};
    ch_a_cfg.speed_mode = LEDC_MODE;
    ch_a_cfg.channel    = CH_A;
    ch_a_cfg.timer_sel  = LEDC_TIMER;
    ch_a_cfg.intr_type  = LEDC_INTR_DISABLE;
    ch_a_cfg.gpio_num   = PIEZO_PIN_A;
    ch_a_cfg.duty       = 0;
    ch_a_cfg.hpoint     = 0;
    ledc_channel_config(&ch_a_cfg);

    // Channel B — complementary phase
    ledc_channel_config_t ch_b_cfg = {};
    ch_b_cfg.speed_mode = LEDC_MODE;
    ch_b_cfg.channel    = CH_B;
    ch_b_cfg.timer_sel  = LEDC_TIMER;
    ch_b_cfg.intr_type  = LEDC_INTR_DISABLE;
    ch_b_cfg.gpio_num   = PIEZO_PIN_B;
    ch_b_cfg.duty       = 0;
    ch_b_cfg.hpoint     = 0;
    ledc_channel_config(&ch_b_cfg);
}

void PiezoDriver::setFrequency(uint32_t hz) {
    if (hz == 0) {
        silence();
        return;
    }
    ledc_set_freq(LEDC_MODE, LEDC_TIMER, hz);
}

void PiezoDriver::setDuty(uint8_t duty) {
    // Map 0-255 → 0-512 (max 50% duty for push-pull)
    uint32_t mapped = ((uint32_t)duty * 512) / 255;
    if (mapped > 512) mapped = 512;

    ledc_set_duty(LEDC_MODE, CH_A, mapped);
    ledc_update_duty(LEDC_MODE, CH_A);

    // Complement: when A is high, B is low and vice versa
    uint32_t complement = MAX_DUTY - mapped;
    ledc_set_duty(LEDC_MODE, CH_B, complement);
    ledc_update_duty(LEDC_MODE, CH_B);
}

void PiezoDriver::silence() {
    ledc_set_duty(LEDC_MODE, CH_A, 0);
    ledc_update_duty(LEDC_MODE, CH_A);
    ledc_set_duty(LEDC_MODE, CH_B, 0);
    ledc_update_duty(LEDC_MODE, CH_B);
}
