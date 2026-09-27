#pragma once

#include "logic/SharedState.h"

namespace mpgd {

class JogController;

// Periodic jog processor (polling.jog_hz, 10 ms by default).
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
