#pragma once

#include "usb/XhcProtocol.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace mpgd {

// Current controller/machine state, refreshed by StateReader (display thread).
struct MachineState {
    double workX = 0.0, workY = 0.0, workZ = 0.0;
    double motorX = 0.0, motorY = 0.0, motorZ = 0.0;
    double feed = 0.0;       // current feed (units/sec, InfoSpeed)
    double spindle = 0.0;    // spindle speed (RPS, InfoSpindle)
    unsigned jogPot = 0;
    bool controllerReady = false;
    bool idle = true;
    bool estop = false;
    bool running = false;    // controller executing a program
    bool paused = false;     // program paused
    bool initialized = false;
};

// Latest pendant input, consumed by the jog/display/button threads.
struct PendantState {
    uint8_t axisCode = 0;    // raw rotary selector position
    int8_t lastJogDelta = 0; // most recent wheel delta (debug)
    bool connected = false;
    bool sleeping = false;
    // Accumulated wheel counts since last consume (jog thread drains this).
    std::atomic<int> jogCounts{0};
};

// Central shared state. Guarded by `mutex`; the two atomics may be touched
// without the lock where indicated.
class SharedState {
public:
    std::mutex mutex;

    MachineState machine;
    PendantState pendant;

    // Jog configuration/state.
    bool jogEnabled{true};           // false in attach mode
    // mm per wheel count; set from jogging.step_sizes by ButtonHandler and
    // cycled by the `step_size` action.
    double stepSize{0.01};
    int stepSizeIndex{1};

    // Process-wide shutdown request (no lock needed).
    std::atomic<bool> shutdown{false};

    // True while axis rotary is OFF (skip display updates, FR-04.5).
    bool axisOff() const {
        return pendant.axisCode == xhc::kAxisOff;
    }

    // Current selected axis index for jogging (0=X,1=Y,2=Z,3=A); -1 when the
    // rotary is not on an axis position.
    int selectedAxis() const {
        switch (pendant.axisCode) {
            case xhc::kAxisX: return 0;
            case xhc::kAxisY: return 1;
            case xhc::kAxisZ: return 2;
            case xhc::kAxisA: return 3;
            default: return -1;
        }
    }

    // True when the rotary selects feed/spindle override.
    bool feedOverrideSelected() const { return pendant.axisCode == xhc::kAxisFeed; }
    bool spindleOverrideSelected() const { return pendant.axisCode == xhc::kAxisSpindle; }
};

} // namespace mpgd
