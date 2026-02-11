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
#define DEBUG_MENU_TIMEOUT_S 30

// Mesh config
#define MESH_MAX_NODES       16
#define MESH_CHANNEL         1
#define MESH_MAX_LAYER       4

#endif //HPP_BSP_HPP
