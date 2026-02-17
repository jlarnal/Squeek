#ifndef FTM_SCHEDULER_H
#define FTM_SCHEDULER_H

#include "bsp.hpp"
#include <stdint.h>

// FTM pair priority levels
enum FtmPriority : uint8_t {
    FTM_PRIO_NEW_NODE    = 0,  // P0: no position yet
    FTM_PRIO_RESIDUAL    = 1,  // P1: high solver residual
    FTM_PRIO_MOVEMENT    = 2,  // P2: RSSI/Kalman detected movement
    FTM_PRIO_STALE       = 3,  // P3: staleness timeout
    FTM_PRIO_SWEEP       = 4,  // P4: periodic full sweep
};

struct FtmQueueItem {
    uint8_t     nodeA_idx;
    uint8_t     nodeB_idx;
    FtmPriority priority;
    uint32_t    queued_ms;   // millis() when queued
};

// Pair execution state machine
enum FtmPairState : uint8_t {
    FTM_PAIR_IDLE = 0,
    FTM_PAIR_WAKE_SENT,
    FTM_PAIR_WAITING_READY,
    FTM_PAIR_GO_SENT,
    FTM_PAIR_WAITING_RESULT,
};

class FtmScheduler {
public:
    static void init();
    static void shutdown();

    /// Queue a pair for FTM measurement
    static void enqueuePair(uint8_t nodeA_idx, uint8_t nodeB_idx, FtmPriority prio);

    /// Queue all N(N-1)/2 pairs (full sweep)
    static void enqueueFullSweep();

    /// Queue measurements for a new node against existing anchors
    static void enqueueNewNode(uint8_t node_idx);

    /// Called when FTM_READY is received from a node
    static void onFtmReady(const uint8_t* mac);

    /// Called when FTM_RESULT is received
    static void onFtmResult(const uint8_t* initiator, const uint8_t* responder,
                            float distance_cm, uint8_t status);

    /// Trigger position solve after measurements complete
    static void triggerSolve();

    /// Broadcast computed positions to all nodes
    static void broadcastPositions();

    /// Check if scheduler is actively running measurements
    static bool isActive();

    /// Debug: print queue state
    static void print();

private:
    FtmScheduler() = delete;
};

#endif // FTM_SCHEDULER_H
