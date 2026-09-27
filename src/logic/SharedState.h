#pragma once

#include "usb/XhcProtocol.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace mpgd {

// Current controller/machine state, refreshed by StateReader (state thread).
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

// Consistent copy of the lock-guarded part of SharedState. Readers take one
// with SharedState::snapshot() and then work on the copy without the lock,
// so derived values (selected axis, e-stop, step size) always come from the
// same instant.
struct StateSnapshot {
    MachineState machine;
    uint8_t axisCode = 0;
    bool pendantConnected = false;
    bool pendantSleeping = false;
    bool jogEnabled = true;
    double stepSize = 0.01;
    int stepSizeIndex = 1;

    // True while axis rotary is OFF (skip display updates, FR-04.5).
    bool axisOff() const { return axisCode == xhc::kAxisOff; }

    // Current selected axis index for jogging (0=X,1=Y,2=Z,3=A); -1 when the
    // rotary is not on an axis position.
    int selectedAxis() const {
        switch (axisCode) {
            case xhc::kAxisX: return 0;
            case xhc::kAxisY: return 1;
            case xhc::kAxisZ: return 2;
            case xhc::kAxisA: return 3;
            default: return -1;
        }
    }

    // True when the rotary selects feed/spindle override.
    bool feedOverrideSelected() const { return axisCode == xhc::kAxisFeed; }
    bool spindleOverrideSelected() const { return axisCode == xhc::kAxisSpindle; }
};

// Central shared state. Every non-atomic member is guarded by `mutex`:
// writers lock it directly, readers use snapshot(). The derived helpers live
// on StateSnapshot only, so they cannot be called on live, unlocked state.
class SharedState {
public:
    mutable std::mutex mutex;

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

    // Copies the guarded state under the lock.
    StateSnapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mutex);
        StateSnapshot s;
        s.machine = machine;
        s.axisCode = pendant.axisCode;
        s.pendantConnected = pendant.connected;
        s.pendantSleeping = pendant.sleeping;
        s.jogEnabled = jogEnabled;
        s.stepSize = stepSize;
        s.stepSizeIndex = stepSizeIndex;
        return s;
    }
};

} // namespace mpgd
