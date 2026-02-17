#ifndef POSITION_SOLVER_H
#define POSITION_SOLVER_H

#include <stdint.h>

class PositionSolver {
public:
    static void init();

    /// Run MDS + Kalman on current PeerTable distances → update positions.
    static void solve();

    /// Reset Kalman state (e.g. after topology change).
    static void reset();

private:
    PositionSolver() = delete;
};

#endif // POSITION_SOLVER_H
