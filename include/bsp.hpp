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

#define SQ_LIGHT_SLEEP(ms)   delay(ms)
#define SQ_DEEP_SLEEP(ms)    do { Serial.println("[DBG] deep-sleep suppressed"); delay(ms); } while(0)
#define SQ_POWER_DELAY(ms)   delay(ms)

#else // release

#define SQ_LIGHT_SLEEP(ms)   do { esp_sleep_enable_timer_wakeup((uint64_t)(ms) * 1000ULL); esp_light_sleep_start(); } while(0)
#define SQ_DEEP_SLEEP(ms)    do { esp_sleep_enable_timer_wakeup((uint64_t)(ms) * 1000ULL); esp_deep_sleep_start(); } while(0)
#define SQ_POWER_DELAY(ms)   delay(ms)

#endif // DEBUG_MENU_ENABLED

// Mesh config
#define MESH_MAX_NODES       16
#define MESH_CHANNEL         1
#define MESH_MAX_LAYER       4

// Election weights
#define ELECT_BATTERY_FLOOR_MV 2900  // below this: disqualified from Gateway
#define ELECT_W_BATTERY      10
#define ELECT_W_ADJACENCY    5000
#define ELECT_W_TENURE       8000

// Election timing
#define ELECT_SETTLE_MS      3000    // wait for mesh to stabilize before election
#define ELECT_TIMEOUT_MS     15000   // fallback: current root keeps Gateway

// Mesh retry
#define MESH_RETRY_DELAY_MS  2000
#define MESH_MAX_RETRIES     10
#define MESH_REELECT_SLEEP_MS 5000   // sleep before reboot on gateway loss

#endif //HPP_BSP_HPP
