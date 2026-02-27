#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include <stdint.h>

enum OrchMode : uint8_t {
    ORCH_OFF       = 0,
    ORCH_TRAVEL    = 1,
    ORCH_RANDOM    = 2,
    ORCH_SEQUENCE  = 3,
    ORCH_SCHEDULED = 4,
};

enum TravelOrder : uint8_t {
    TRAVEL_NEAREST = 0,
    TRAVEL_AXIS    = 1,
    TRAVEL_RANDOM  = 2,
};

struct SeqStep {
    uint8_t  node_index;   // PeerTable index
    uint8_t  tone_index;   // ToneLibrary index
    uint16_t delay_ms;     // delay AFTER this step
};

class Print;

class Orchestrator {
public:
    Orchestrator() = delete;

    static void init();
    static void stop();

    // Gateway controls
    static void setMode(OrchMode mode);
    static OrchMode getMode();
    static void setTravelOrder(TravelOrder order);
    static TravelOrder getTravelOrder();

    // Peer-side handlers (called from mesh dispatch)
    static void onPlayCmd(uint8_t tone_index);
    static void onModeChange(uint8_t mode);

    // Sequence editing
    static void addSequenceStep(uint8_t node_idx, uint8_t tone_idx, uint16_t delay_ms);
    static void clearSequence();
    static void loadSequence();
    static void saveSequence();
    static uint8_t sequenceCount();
    static const SeqStep* sequenceSteps();

    // Scheduled triggers
    static void scheduleRelative(uint32_t delay_ms, OrchMode mode);
    static void cancelSchedule();

    // Status
    static void printStatus(Print& out);

private:
    static void orchTask(void* param);
};

#endif // ORCHESTRATOR_H
