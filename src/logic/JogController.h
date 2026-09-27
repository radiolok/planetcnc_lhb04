#pragma once

#include "config/ConfigManager.h"
#include "logic/SharedState.h"

#include <chrono>

namespace mpgd {

class ITngApi;

// Converts accumulated MPG wheel counts into smooth position-following motion.
// The wheel updates a target coordinate and a velocity servo drives the axis
// toward it via continuous Jog (mm/s). The velocity is capped by the braking
// distance so the axis decelerates to zero at the target, and hysteresis (with
// a stop deadband larger than the controller's minimum-jog braking distance)
// prevents a limit cycle. Also handles feed/spindle override.
// Runs on the jog thread (10 ms tick).
class JogController {
public:
    JogController(ITngApi& api, SharedState& state, const JoggingConfig& cfg);

    // One periodic tick. Drains the wheel-count accumulator, updates the target
    // and drives the servo. Safe to call repeatedly even when the TNG API is
    // not initialized or jogging is disabled.
    void tick();

    // Immediately stops any active motion and clears accumulated state.
    void stopNow();

private:
    int drainCounts();
    void updateTarget(int axis, int counts);
    void runServo(int axis, bool freshCounts);
    void stopServo();
    void resetTargets();
    void processOverride(bool spindle, int counts);

    ITngApi& api_;
    SharedState& state_;
    const JoggingConfig& cfg_;

    // Per-axis target positions (motor coordinates). NaN = not yet known;
    // initialized lazily from the current motor position on first use.
    // Cleared whenever the servo stops, the axis selection changes, or the
    // machine is moved by anything else (program, homing, TNG GUI jog, e-stop),
    // so a stale target can never drive the axis back to an old position.
    double target_[6];
    int lastAxis_ = -1;
    // Until this time the machine reporting "not idle" is attributed to our
    // own servo decelerating after a stop rather than to an external move.
    std::chrono::steady_clock::time_point ownMotionUntil_{};
    bool servoActive_ = false;
    int servoAxis_ = -1;
    // Last commanded velocity (mm/s). Used to avoid re-issuing Jog() every
    // tick: the controller keeps jogging at the last velocity, and re-issuing
    // on every tick makes it re-ramp and overshoot (limit-cycle oscillation).
    double lastVel_ = 0.0;
};

} // namespace mpgd
