#include "audio_engine.h"
#include <driver/gptimer.h>
#include <esp_attr.h>

// --- File-scope playback state ---
static IAudioOutput*        s_output       = nullptr;
static const ToneSequence*  s_current      = nullptr;
static uint8_t              s_seg_idx      = 0;
static uint16_t             s_tick         = 0;
static uint16_t             s_seg_ticks    = 0;
static uint8_t              s_repeat_cnt   = 0;
static volatile bool        s_playing      = false;
static gptimer_handle_t     s_timer        = nullptr;

// ISR tick rate
static constexpr uint32_t TICK_HZ = 200;

// --- GPTimer ISR: envelope interpolation at 200 Hz ---
static bool IRAM_ATTR onTimerAlarm(gptimer_handle_t timer,
                                   const gptimer_alarm_event_data_t* edata,
                                   void* user_ctx)
{
    (void)timer; (void)edata; (void)user_ctx;
    if (!s_playing || !s_current || !s_output) return false;

    const ToneSegment& seg = s_current->segments[s_seg_idx];

    if (s_seg_ticks == 0) {
        // Silence segment (freq == 0)
        s_output->silence();
    } else {
        // Fixed-point linear interpolation: (start * (total-t) + end * t) / total
        uint32_t freq = ((uint32_t)seg.freq_start_hz * (s_seg_ticks - s_tick)
                       + (uint32_t)seg.freq_end_hz   * s_tick)
                      / s_seg_ticks;
        uint32_t duty = ((uint32_t)seg.duty_start * (s_seg_ticks - s_tick)
                       + (uint32_t)seg.duty_end   * s_tick)
                      / s_seg_ticks;

        if (freq > 0) {
            s_output->setFrequency(freq);
            s_output->setDuty((uint8_t)duty);
        } else {
            s_output->silence();
        }
    }

    s_tick++;
    if (s_tick >= s_seg_ticks) {
        // Advance to next segment
        s_tick = 0;
        s_seg_idx++;
        if (s_seg_idx >= s_current->count) {
            // Sequence ended — check repeats
            if (s_current->repeats == 255) {
                // Loop forever
                s_seg_idx = 0;
            } else if (s_repeat_cnt < s_current->repeats) {
                s_repeat_cnt++;
                s_seg_idx = 0;
            } else {
                // Done
                s_playing = false;
                s_output->silence();
                return false;
            }
        }
        // Precompute ticks for new segment
        s_seg_ticks = (uint16_t)(((uint32_t)s_current->segments[s_seg_idx].duration_ms * TICK_HZ) / 1000);
        if (s_seg_ticks == 0) s_seg_ticks = 1;
    }

    return false;  // no need to yield
}

// --- Public API ---

void AudioEngine::init(IAudioOutput* output) {
    s_output = output;

    // Configure GPTimer: 1 MHz resolution, alarm at 5000 counts = 200 Hz
    gptimer_config_t timer_cfg = {};
    timer_cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timer_cfg.direction = GPTIMER_COUNT_UP;
    timer_cfg.resolution_hz = 1000000;  // 1 MHz
    gptimer_new_timer(&timer_cfg, &s_timer);

    // Register alarm callback
    gptimer_event_callbacks_t cbs = {};
    cbs.on_alarm = onTimerAlarm;
    gptimer_register_event_callbacks(s_timer, &cbs, nullptr);

    // Configure alarm: period = 5000 us = 200 Hz, auto-reload
    gptimer_alarm_config_t alarm_cfg = {};
    alarm_cfg.alarm_count = 5000;
    alarm_cfg.reload_count = 0;
    alarm_cfg.flags.auto_reload_on_alarm = true;
    gptimer_set_alarm_action(s_timer, &alarm_cfg);

    gptimer_enable(s_timer);
}

void AudioEngine::play(const ToneSequence* seq) {
    if (!seq || !s_output || seq->count == 0) return;

    // Stop any current playback
    s_playing = false;
    if (s_timer) gptimer_stop(s_timer);

    s_current    = seq;
    s_seg_idx    = 0;
    s_tick       = 0;
    s_repeat_cnt = 0;

    // Precompute ticks for first segment
    s_seg_ticks = (uint16_t)(((uint32_t)seq->segments[0].duration_ms * TICK_HZ) / 1000);
    if (s_seg_ticks == 0) s_seg_ticks = 1;

    s_playing = true;
    if (s_timer) gptimer_start(s_timer);
}

void AudioEngine::stop() {
    s_playing = false;
    if (s_timer) gptimer_stop(s_timer);
    if (s_output) s_output->silence();
}

bool AudioEngine::isPlaying() {
    return s_playing;
}
