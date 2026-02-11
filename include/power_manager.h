#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

void     power_init();
uint32_t power_battery_raw();
uint32_t power_battery_mv();
bool     power_is_low_battery();
bool     power_is_critical_battery();
void     power_enter_light_sleep(uint32_t seconds);
void     power_enter_deep_sleep(uint32_t seconds);

#endif // POWER_MANAGER_H
