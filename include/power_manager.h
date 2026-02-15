#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdint.h>

class PowerManager {
public:
    static void     init();
    static uint32_t batteryRaw();
    static uint32_t batteryMv();
    static bool     isLowBattery();
    static bool     isCriticalBattery();
    static void     lightSleep(uint32_t seconds);
    static void     deepSleep(uint32_t seconds);

private:
    PowerManager() = delete;
};

#endif // POWER_MANAGER_H
