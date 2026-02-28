#ifndef HPP_BSP_HPP
#define HPP_BSP_HPP

#include <Arduino.h>
#ifdef LED_BUILTIN
#undef LED_BUILTIN
#endif

#define SQUEEK_VERSION       "0.1.0"
#define SQUEEK_NAME          "Squeek"

// GPIO pins
#define LED_BUILTIN          GPIO_NUM_15
#define RBG_BUILTIN          GPIO_NUM_8

// Piezo buzzer (push-pull, opposed phases)
constexpr gpio_num_t PIEZO_PIN_A = GPIO_NUM_22;  // push-pull positive
constexpr gpio_num_t PIEZO_PIN_B = GPIO_NUM_23;  // push-pull complement

// Battery ADC
#define BATTERY_ADC_PIN      GPIO_NUM_2
#define BATTERY_ADC_CHANNEL  ADC_CHANNEL_2

// Voltage divider constants
#define VDIV_R1              100000.0f
#define VDIV_R2              100000.0f
#define VDIV_RATIO           ((VDIV_R1 + VDIV_R2) / VDIV_R2)

// Low battery thresholds (LiPo)
#define BATTERY_LOW_MV       3300
#define BATTERY_CRITICAL_MV  3100

// Debug menu
#define DEBUG_MENU_ENABLED

// ---------------------------------------------------------------------------
// Power macros — use these instead of raw sleep/delay for power-saving waits.
// In debug builds they stay awake so Serial and JTAG keep working.
// ---------------------------------------------------------------------------
#ifdef DEBUG_MENU_ENABLED

#define SQ_LIGHT_SLEEP(ms)   vTaskDelay(pdMS_TO_TICKS(ms))
#define SQ_DEEP_SLEEP(ms)    do { Serial.println("[DBG] deep-sleep suppressed"); vTaskDelay(pdMS_TO_TICKS(ms)); } while(0)
#define SQ_POWER_DELAY(ms)   vTaskDelay(pdMS_TO_TICKS(ms))

#else // release

#define SQ_LIGHT_SLEEP(ms)   do { esp_sleep_enable_timer_wakeup((uint64_t)(ms) * 1000ULL); esp_light_sleep_start(); } while(0)
#define SQ_DEEP_SLEEP(ms)    do { esp_sleep_enable_timer_wakeup((uint64_t)(ms) * 1000ULL); esp_deep_sleep_start(); } while(0)
#define SQ_POWER_DELAY(ms)   delay(ms)

#endif // DEBUG_MENU_ENABLED

// NvsConfigManager property defaults (override here to change factory values)
#define NVS_DEFAULT_LEDS_ENABLED        true
#define NVS_DEFAULT_ELECT_W_BATTERY     1.0f
#define NVS_DEFAULT_ELECT_W_ADJACENCY   5.0f
#define NVS_DEFAULT_ELECT_W_TENURE      8.0f
#define NVS_DEFAULT_ELECT_W_LOWBAT_PEN  0.1f
#define NVS_DEFAULT_CLR_INIT            0x00140600   // orange (20,6,0)
#define NVS_DEFAULT_CLR_READY           0x00140F00   // yellow  (20,15,0)
#define NVS_DEFAULT_CLR_GATEWAY         0x00000008   // dim blue   (0,10,15)
#define NVS_DEFAULT_CLR_PEER            0x00000800   // dim green      (0,255,0)
#define NVS_DEFAULT_CLR_DISCONNECTED    0x00200000   // dim red        (255,0,0)

// Phase 2: Heartbeat
#define NVS_DEFAULT_HB_INTERVAL_S      30
#define NVS_DEFAULT_HB_STALE_MULT      3
#define NVS_DEFAULT_REELECT_DELTA_MV   200
#define NVS_DEFAULT_REELECT_COOLDOWN_S 60
#define NVS_DEFAULT_REELECT_DETHRONE_MV 300

// Phase 2: FTM
#define NVS_DEFAULT_FTM_STALE_S        300
#define NVS_DEFAULT_FTM_NEW_ANCHORS    5
#define NVS_DEFAULT_FTM_SAMPLES        8
#define NVS_DEFAULT_FTM_PAIR_TMO_MS    3000
#define NVS_DEFAULT_FTM_SWEEP_INT_S    600
#define NVS_DEFAULT_FTM_KALMAN_PN      0.01f
#define NVS_DEFAULT_FTM_RESP_OFS_CM    0

// Phase 4: Orchestrator
#define NVS_DEFAULT_ORCH_MODE          0
#define NVS_DEFAULT_ORCH_TRAVEL_DELAY  500
#define NVS_DEFAULT_ORCH_RANDOM_MIN    3000
#define NVS_DEFAULT_ORCH_RANDOM_MAX    15000
#define NVS_DEFAULT_ORCH_TONE_INDEX    0
#define NVS_DEFAULT_CSYNC_INTERVAL_S   10

// Phase 5: Web UI
#define NVS_DEFAULT_WEB_ENABLED         true
#define SOFTAP_MAX_CONNECTIONS          4

// Mesh config
#define MESH_MAX_NODES       16
#define MESH_CHANNEL         1
#define MESH_MAX_LAYER       4

// Election
#define ELECT_BATTERY_FLOOR_MV  2900    // below this: heavy score penalty (not disqualifying)

// Election timing
#define ELECT_SETTLE_MS      3000    // wait for mesh to stabilize before election
#define ELECT_TIMEOUT_MS     15000   // fallback: current root keeps Gateway

// Mesh retry
#define MESH_RETRY_DELAY_MS  2000
#define MESH_MAX_RETRIES     10
#define MESH_REELECT_SLEEP_MS 5000   // sleep before reboot on gateway loss

#endif //HPP_BSP_HPP
