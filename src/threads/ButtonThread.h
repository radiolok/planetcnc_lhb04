#pragma once

#include "logic/SharedState.h"

namespace mpgd {

class ButtonHandler;
class ButtonQueue;

// Executes queued button presses off the USB poll thread, so a slow TNG call
// never delays reading pendant input.
class ButtonThread {
public:
    ButtonThread(SharedState& state, ButtonQueue& queue, ButtonHandler& handler);

    // Blocking loop until SharedState::shutdown is set.
    void run();

private:
    SharedState& state_;
    ButtonQueue& queue_;
    ButtonHandler& handler_;
};

} // namespace mpgd
