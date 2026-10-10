#pragma once

#include "config/ConfigManager.h"
#include "logic/SharedState.h"

#include <chrono>
#include <optional>

namespace mpgd {

class ITngApi;

// Converts accumulated MPG wheel counts into absolute position-following
// motion against a frozen "stable reference point" (anchor).
//
// While the controller is idle (machine stationary) the current motor
// coordinate of the selected axis is captured as the anchor and refreshed on
// every tick. When the wheel starts turning, all increments/decrements are
// accumulated relative to that anchor and the resulting absolute target
// (anchor + offset) is sent via PlanetCNC MoveAxis().
//
// The anchor does NOT change while the machine is moving: it is refreshed only
// once the controller reports idle again (the axis has stopped at the new
// point). No current-position feedback (target - current) is used while
// moving - that comparison previously drove the system into self-oscillation.
//
// Every pending target is dropped on e-stop, jog disable, axis change, a
// paused program, or motion that was already running when the wheel turned
// (program, homing, MDI, TNG GUI jog), so a stale target can never drive the
// axis back to an old position. TNG reports our own MoveAxis as running too,
// so that flag does not abort an active wheel move. A MoveAxis re-issue the
// controller rejects mid-move is retried instead of dropping the offset.
//
// Feed/spindle override is handled when the rotary selects those positions.
// Runs on the jog thread (10 ms tick).
class JogController {
public:
    JogController(ITngApi& api, SharedState& state, const JoggingConfig& cfg);

    // One periodic tick. Drains the wheel-count accumulator and, for the
    // selected axis, keeps the anchor synced while idle, accumulates wheel
    // counts relative to the anchor, issues MoveAxis when appropriate and
    // settles back to a new anchor once the machine is idle again. Safe to
    // call repeatedly even when the TNG API is not initialized or jogging is
    // disabled.
    void tick();

    // Immediately stops any active motion and clears accumulated state.
    void stopNow();

private:
    int drainCounts();
    double readMotorPosition(int axis);
    void syncAnchor(int axis);
    void beginJog(int axis, int counts, double stepSize);
    void issueMove(int axis);
    void settle(int axis);
    void stopMotion();
    void reset();
    void processOverride(bool spindle, int counts);

    ITngApi& api_;
    SharedState& state_;
    const JoggingConfig& cfg_;

    // Selected axis on the previous tick; a change drops all motion state.
    int lastAxis_ = -1;
    // Stable reference point (motor coordinate) of the selected axis.
    // NaN = not captured. Refreshed only while idle; frozen during a move.
    double anchor_;
    // Accumulated wheel offset in mm since the anchor (reset on settle/sync).
    double offset_ = 0.0;
    // True while a wheel-driven move is in flight (anchor frozen).
    bool active_ = false;
    // Last absolute target passed to MoveAxis (NaN = none). Used to skip
    // redundant re-issues.
    double sentTarget_;
    // Per-axis flag so the "motor position unavailable" warning is emitted
    // once per outage (not on every 10 ms tick).
    bool warnedNaN_[6] = {};
    // Time of the last MoveAxis command; rate-limits re-issuing and backs the
    // settle timeout.
    std::chrono::steady_clock::time_point lastMove_{};
    // Start of the current run of rejected MoveAxis re-issues (empty = none).
    std::optional<std::chrono::steady_clock::time_point> failingSince_;
};

} // namespace mpgd
