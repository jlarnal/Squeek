#ifndef CLOCK_SYNC_H
#define CLOCK_SYNC_H

#include <stdint.h>

class ClockSync {
public:
    ClockSync() = delete;
    static void init();
    static void stop();
    static void onSyncReceived(uint32_t gateway_ms);
    static uint32_t meshTime();
    static bool isSynced();
};

#endif // CLOCK_SYNC_H
