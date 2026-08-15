#pragma once

#include "logic/SharedState.h"

namespace mpgd {

class JogController;

// Periodic jog processor (10 ms cadence).
class JogThread {
public:
    JogThread(SharedState& state, JogController& controller, int periodMs);

    // Blocking loop until SharedState::shutdown is set.
    void run();

private:
    SharedState& state_;
    JogController& controller_;
    int periodMs_;
};

} // namespace mpgd
