#include "logic/JogController.h"

#include "logic/JogMath.h"
#include "planetcnc/ITngApi.h"
#include "utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace mpgd {

namespace {
// Position-following parameters.
constexpr double kEpsilonMm = 0.0005;  // 0.5 um: skip redundant re-issues
constexpr std::chrono::milliseconds kReissueMin{20};  // coalesce a continuous spin
// Settle timeout: once the wheel is quiet and the controller reports idle for
// this long, the axis is considered stopped. Must exceed the StateReader
// sampling period (50 ms) so a stale idle flag can never trigger an early
// settle while the machine is actually still moving.
constexpr std::chrono::milliseconds kSettleMin{150};
// MoveAxis may be rejected while a previous move is still being planned or
// executed. A rejected re-issue keeps the accumulated offset and is retried;
// the jog is abandoned only when the controller keeps refusing for this long.
constexpr std::chrono::milliseconds kRetryMax{500};
// On settle, the commanded target becomes the new anchor when the reached
// position is within this distance of it (motor resolution / rounding), so
// sub-resolution wheel steps are never lost. A larger gap means the axis was
// stopped short (soft limit, external stop) and the reached position is used.
constexpr double kSettleTolMm = 0.01;
} // namespace

JogController::JogController(ITngApi& api, SharedState& state, const JoggingConfig& cfg)
    : api_(api), state_(state), cfg_(cfg) {
    reset();
}

int JogController::drainCounts() {
    int counts = state_.pendant.jogCounts.exchange(0);
    int clamped = clampAccumulatedCounts(counts);
    if (clamped != counts) {
        logWarn("jog: accumulated counts %d exceeded clamp bound", counts);
    }
    return clamped;
}

double JogController::readMotorPosition(int axis) {
    const double pos = api_.infoMotorPosition(axis);
    if (!std::isfinite(pos)) {
        if (!warnedNaN_[axis]) {
            logWarn("jog: motor position for axis %d is unavailable (NaN) - "
                    "machine homed/connected and motors enabled? jog disabled "
                    "for this axis until a valid position is reported",
                    axis);
            warnedNaN_[axis] = true;
        }
    } else {
        warnedNaN_[axis] = false;
    }
    return pos;
}

void JogController::stopMotion() {
    if (active_) api_.jogStop();
    reset();
}

void JogController::reset() {
    anchor_ = std::numeric_limits<double>::quiet_NaN();
    offset_ = 0.0;
    active_ = false;
    sentTarget_ = std::numeric_limits<double>::quiet_NaN();
    failingSince_.reset();
}

void JogController::stopNow() {
    state_.pendant.jogCounts.store(0);
    api_.jogStop();
    reset();
}

void JogController::tick() {
    const int counts = drainCounts();

    const StateSnapshot snap = state_.snapshot();
    const bool jogEnabled = snap.jogEnabled;
    bool estop = snap.machine.estop;
    const bool running = snap.machine.running;
    const bool paused = snap.machine.paused;
    const bool idle = snap.machine.idle;
    const int axis = snap.selectedAxis();
    const bool feedSel = snap.feedOverrideSelected();
    const bool spindleSel = snap.spindleOverrideSelected();
    const double stepSize = snap.stepSize > 0.0 ? snap.stepSize : 0.001;

    // The shared e-stop flag is refreshed at the state-reader rate only.
    // Before issuing or continuing motion, ask the controller directly so
    // an e-stop is never acted on late.
    if (!estop && jogEnabled && axis >= 0 && (active_ || counts != 0)) {
        estop = api_.isEStop();
    }

    if (estop || !jogEnabled) {
        if (counts != 0) {
            logDebug("jog: %d counts dropped (estop=%d jogEnabled=%d)", counts, estop, jogEnabled);
        }
        stopMotion();
        return;
    }

    if (axis != lastAxis_) {
        // Selection changed: drop the move and the anchor; the axes may be
        // moved by other means while not selected.
        stopMotion();
        lastAxis_ = axis;
    }

    if (axis >= 0) {
        // Never jog while a program is paused, and never start a jog while the
        // controller executes anything. TNG reports our own MoveAxis as
        // "running" too (IsStop is false for any motion), so an active wheel
        // move is not aborted by it.
        if (paused || (running && !active_)) {
            if (counts != 0) {
                logDebug("jog: %d counts dropped (running=%d paused=%d)", counts, running, paused);
            }
            stopMotion();
            return;
        }
        if (active_) {
            // Moving: the anchor stays frozen. Extend the target when the
            // wheel keeps turning, and settle only once the machine is idle
            // again (no current-position feedback is used here).
            // Settle only when the wheel is quiet and the last target has
            // actually been sent, so no wheel input is ever dropped.
            if (counts != 0) offset_ += static_cast<double>(counts) * stepSize;
            issueMove(axis);
            if (!active_) return;  // MoveAxis failed and stopped
            const bool pending = std::fabs(anchor_ + offset_ - sentTarget_) >= kEpsilonMm;
            if (counts == 0 && !pending && idle &&
                std::chrono::steady_clock::now() - lastMove_ >= kSettleMin) {
                settle(axis);
            }
        } else if (!idle) {
            // Machine busy with motion that is not ours (homing, MDI, TNG GUI
            // jog, or our own deceleration after a stop): discard the anchor
            // and the wheel input; it is re-captured once idle again.
            if (counts != 0) logDebug("jog: %d counts dropped (machine not idle)", counts);
            anchor_ = std::numeric_limits<double>::quiet_NaN();
        } else {
            // Stationary: keep the anchor synced to the current position so a
            // future wheel turn starts from the latest stable point.
            syncAnchor(axis);
            if (counts != 0) beginJog(axis, counts, stepSize);
        }
    } else {
        if (feedSel && counts != 0) {
            processOverride(/*spindle=*/false, counts);
        } else if (spindleSel && counts != 0) {
            processOverride(/*spindle=*/true, counts);
        }
    }
}

void JogController::syncAnchor(int axis) {
    if (axis < 0 || axis >= 6) return;
    const double pos = readMotorPosition(axis);
    offset_ = 0.0;
    // Keep a settled anchor that is within motor resolution of the reported
    // position (see settle()); re-capture after any real external move.
    if (std::isfinite(pos) && std::isfinite(anchor_) && std::fabs(pos - anchor_) <= kSettleTolMm) {
        return;
    }
    anchor_ = pos;  // NaN when unavailable: jog stays disabled until valid
}

void JogController::beginJog(int axis, int counts, double stepSize) {
    if (axis < 0 || axis >= 6) return;
    if (!std::isfinite(anchor_)) {
        logDebug("jog: %d counts dropped (no anchor for axis %d)", counts, axis);
        return;
    }
    active_ = true;
    offset_ = static_cast<double>(counts) * stepSize;
    issueMove(axis);
}

void JogController::issueMove(int axis) {
    if (axis < 0 || axis >= 6) return;
    if (!std::isfinite(anchor_)) return;

    const double target = anchor_ + offset_;

    // Re-issue only when the target has actually moved since the last command
    // (so a single wheel click always moves the axis) and no more often than
    // kReissueMin so a continuous spin extends one smooth move instead of
    // re-planning on every tick (which caused stop-start jerks).
    if (std::isfinite(sentTarget_)) {
        if (std::fabs(target - sentTarget_) < kEpsilonMm) return;
        if (std::chrono::steady_clock::now() - lastMove_ < kReissueMin) return;
    }

    // MoveAxis speed: actual mm/min = value / 10.
    const double speed = std::max(1.0, cfg_.maxSpeed) * 10.0;
    const auto now = std::chrono::steady_clock::now();
    if (api_.moveAxis(speed, axis, target)) {
        logDebug("jog: MoveAxis axis=%d target=%.4f (anchor=%.4f offset=%+.4f)", axis, target, anchor_, offset_);
        sentTarget_ = target;
        lastMove_ = now;
        failingSince_.reset();
    } else if (std::isfinite(sentTarget_) && (!failingSince_ || now - *failingSince_ < kRetryMax)) {
        // A move is in flight and the controller rejected the re-issue: keep
        // the offset and retry on a later tick so no wheel input is lost.
        if (!failingSince_) failingSince_ = now;
        lastMove_ = now;
        logDebug("jog: MoveAxis axis=%d target=%.4f rejected; retrying", axis, target);
    } else {
        // Never carry on from a command the controller did not accept.
        logWarn("jog: MoveAxis on axis %d failed; stopping", axis);
        stopMotion();
    }
}

void JogController::settle(int axis) {
    if (axis < 0 || axis >= 6) return;
    // The axis stopped at the new point: adopt it as the new stable reference
    // and reset the accumulated offset. When the axis reached the commanded
    // target (within motor resolution) the exact target is kept, so steps
    // finer than the motor resolution accumulate instead of being rounded
    // away. If it stopped short, or the position cannot be read, the reached
    // position / commanded target respectively is used.
    const double pos = readMotorPosition(axis);
    if (!std::isfinite(pos) || std::fabs(pos - sentTarget_) <= kSettleTolMm) {
        anchor_ = sentTarget_;
    } else {
        anchor_ = pos;
    }
    logDebug("jog: settled axis=%d at %.4f (target %.4f)", axis, anchor_, sentTarget_);
    offset_ = 0.0;
    active_ = false;
    sentTarget_ = std::numeric_limits<double>::quiet_NaN();
    failingSince_.reset();
}

void JogController::processOverride(bool spindle, int counts) {
    const std::string& param = spindle ? cfg_.spindleOverrideParam : cfg_.feedOverrideParam;
    const std::optional<double> current = api_.getParam(param);
    if (!current) {
        // Never compute an override from a value that was not read.
        logWarn("override: GetParam(%s) failed; wheel input ignored", param.c_str());
        return;
    }
    double delta = counts * (cfg_.overrideStep / 100.0);
    double value = std::clamp(*current + delta, kOverrideMin, kOverrideMax);
    if (api_.setParam(param, value)) {
        logDebug("override: %s %+.2f%% -> %.1f%%", param.c_str(), delta * 100.0, value * 100.0);
    } else {
        logWarn("override: SetParam(%s) failed", param.c_str());
    }
}

} // namespace mpgd
