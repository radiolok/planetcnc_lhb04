#pragma once

#include "config/ConfigManager.h"
#include "logic/SharedState.h"

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
    void runServo(int axis);
    void stopServo();
    void processOverride(bool spindle, int counts);

    ITngApi& api_;
    SharedState& state_;
    const JoggingConfig& cfg_;

    // Per-axis target positions (motor coordinates). NaN = not yet known;
    // initialized lazily from the current motor position on first use.
    double target_[6];
    bool servoActive_ = false;
    int servoAxis_ = -1;
    // Last commanded velocity (mm/s). Used to avoid re-issuing Jog() every
    // tick: the controller keeps jogging at the last velocity, and re-issuing
    // on every tick makes it re-ramp and overshoot (limit-cycle oscillation).
    double lastVel_ = 0.0;
};

} // namespace mpgd
