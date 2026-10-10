#pragma once

#include "logic/SharedState.h"

namespace mpgd {

class DisplayUpdater;
class StateReader;

namespace usb {
class HidDevice;
}

// Periodic machine-state refresh and LCD update thread (polling.display_hz).
class DisplayThread {
public:
    DisplayThread(SharedState& state, usb::HidDevice& device, DisplayUpdater& updater,
                  StateReader& stateReader, int periodMs);

    // Blocking loop until SharedState::shutdown is set.
    void run();

private:
    SharedState& state_;
    usb::HidDevice& device_;
    DisplayUpdater& updater_;
    StateReader& stateReader_;
    int periodMs_;
};

} // namespace mpgd
