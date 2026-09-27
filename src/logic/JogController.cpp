#include "logic/JogController.h"

#include "logic/JogMath.h"
#include "planetcnc/ITngApi.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mpgd {

namespace {
constexpr double kOverrideMin = 0.0;
constexpr double kOverrideMax = 2.5;
// Velocity servo parameters.
//
// PlanetCNC Jog() treats the velocity argument as a MULTIPLIER of the jog
// speed (_jog_speed, mm/s): actual mm/s = value * jogSpeed, capped at the
// controller max speed (validated on Mk3/4: value 0.5 -> 6 mm/s, value 5 ->
// 60 mm/s with _jog_speed = 12). The servo works in mm/s and divides by
// cfg_.jogSpeed when issuing Jog().
constexpr double kServoGain = 5.0;          // 1/s: vel = error * gain
constexpr double kServoMaxDecel = 200.0;    // mm/s^2 (== _motion_maxdec)
constexpr double kServoStopDeadbandMm = 0.05;
constexpr double kServoStartDeadbandMm = 0.15;
// Minimum change in commanded velocity that warrants re-issuing Jog().
constexpr double kServoVelReissueMmS = 1.0;
// After the servo stops, "machine not idle" is treated as our own axis still
// decelerating (not an external move) for this long.
constexpr std::chrono::milliseconds kOwnMotionSettle{500};
} // namespace

JogController::JogController(ITngApi& api, SharedState& state,
                             const JoggingConfig& cfg)
    : api_(api), state_(state), cfg_(cfg) {
    for (double& t : target_) t = std::numeric_limits<double>::quiet_NaN();
}

int JogController::drainCounts() {
    int counts = state_.pendant.jogCounts.exchange(0);
    int clamped = clampAccumulatedCounts(counts);
    if (clamped != counts) {
        logWarn("jog: accumulated counts %d exceeded clamp bound", counts);
    }
    return clamped;
}

void JogController::stopServo() {
    if (servoActive_) {
        api_.jogStop();
        if (servoAxis_ >= 0 && servoAxis_ < 6) {
            target_[servoAxis_] = std::numeric_limits<double>::quiet_NaN();
        }
        servoActive_ = false;
        servoAxis_ = -1;
        lastVel_ = 0.0;
        ownMotionUntil_ = std::chrono::steady_clock::now() + kOwnMotionSettle;
    }
}

void JogController::resetTargets() {
    for (double& t : target_) t = std::numeric_limits<double>::quiet_NaN();
}

void JogController::stopNow() {
    state_.pendant.jogCounts.store(0);
    stopServo();
    resetTargets();
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

    // The shared e-stop flag is refreshed at the state-reader rate only.
    // Before issuing or continuing motion, ask the controller directly so
    // an e-stop is never acted on late.
    if (!estop && jogEnabled && axis >= 0 && (servoActive_ || counts != 0)) {
        estop = api_.isEStop();
    }

    if (estop || !jogEnabled) {
        stopServo();
        resetTargets();
        return;
    }

    if (axis != lastAxis_) {
        // Selection changed: drop the servo and every pending target; the
        // axes may be moved by other means while not selected.
        stopServo();
        resetTargets();
        lastAxis_ = axis;
    }

    if (axis >= 0) {
        // Never jog while a program runs or is paused.
        if (running || paused) {
            stopServo();
            resetTargets();
            return;
        }
        // Machine busy with motion that is not ours (homing, MDI, TNG GUI
        // jog): discard the target and the wheel input.
        if (!idle && !servoActive_ &&
            std::chrono::steady_clock::now() >= ownMotionUntil_) {
            resetTargets();
            return;
        }
        if (counts != 0) updateTarget(axis, counts, snap.stepSize);
        runServo(axis, counts != 0);
    } else {
        if (feedSel && counts != 0) {
            processOverride(/*spindle=*/false, counts);
        } else if (spindleSel && counts != 0) {
            processOverride(/*spindle=*/true, counts);
        }
    }
}

void JogController::updateTarget(int axis, int counts, double stepSize) {
    if (axis < 0 || axis >= 6) return;

    if (stepSize <= 0.0) stepSize = 0.001;

    if (!std::isfinite(target_[axis])) {
        const double pos = api_.infoMotorPosition(axis);
        if (!std::isfinite(pos)) {
            logWarn("jog: cannot read motor position for axis %d", axis);
            return;
        }
        target_[axis] = pos;
    }

    target_[axis] += static_cast<double>(counts) * stepSize;
}

void JogController::runServo(int axis, bool freshCounts) {
    if (axis < 0 || axis >= 6) return;

    // Switching axes: stop the previous axis's servo immediately.
    if (servoActive_ && servoAxis_ != axis) stopServo();

    if (!std::isfinite(target_[axis])) return;

    // Only start following a target on fresh wheel input; an idle servo never
    // starts moving on its own.
    if (!servoActive_ && !freshCounts) return;

    const double current = api_.infoMotorPosition(axis);
    if (!std::isfinite(current)) {
        // Without a position the servo cannot close the loop; the controller
        // would keep jogging at the last velocity forever.
        logWarn("jog: cannot read motor position for axis %d; stopping", axis);
        stopServo();
        target_[axis] = std::numeric_limits<double>::quiet_NaN();
        return;
    }

    const double error = target_[axis] - current;
    const double absErr = std::fabs(error);

    if (servoActive_) {
        if (absErr <= kServoStopDeadbandMm) {
            logInfo("servo stop  axis=%d target=%.3f pos=%.3f err=%.3f",
                    axis, target_[axis], current, error);
            stopServo();
            return;
        }
    } else {
        if (absErr <= kServoStartDeadbandMm) return;
    }

    const double maxVel = std::max(1.0, cfg_.maxSpeed / 60.0);
    // Velocity limited by the braking distance so the axis can decelerate to
    // zero within the remaining error (no overshoot -> no limit cycle).
    double vel = std::min(kServoGain * absErr,
                          std::sqrt(2.0 * kServoMaxDecel * absErr));
    vel = std::min(vel, maxVel);
    vel = std::copysign(vel, error);

    // Only re-issue Jog() when the velocity changes meaningfully. Re-issuing
    // on every tick makes the controller re-ramp and overshoot, which is what
    // drove the limit-cycle oscillation.
    if (servoActive_ && servoAxis_ == axis &&
        std::fabs(vel - lastVel_) < kServoVelReissueMmS) {
        return;
    }

    double v[6] = {0, 0, 0, 0, 0, 0};
    // Jog() velocity argument is a multiplier of the controller's jog speed:
    // actual mm/s = value * jogSpeed. Convert the desired mm/s to the value.
    const double jogSpeed = std::max(0.1, cfg_.jogSpeed);
    v[axis] = vel / jogSpeed;

    bool ok;
    if (axis < 3) {
        ok = api_.jog(false, v[0], v[1], v[2]);
    } else {
        ok = api_.jog9(false, v[0], v[1], v[2], v[3], v[4], v[5], 0, 0, 0);
    }
    if (ok) {
        if (!servoActive_) {
            logInfo("servo start axis=%d target=%.3f pos=%.3f err=%.3f vel=%.2f",
                    axis, target_[axis], current, error, vel);
        }
        servoActive_ = true;
        servoAxis_ = axis;
        lastVel_ = vel;
    } else {
        // A failed update leaves the controller at the previous velocity,
        // which may overshoot the target: stop instead of carrying on.
        logWarn("jog: velocity jog on axis %d failed; stopping", axis);
        stopServo();
        target_[axis] = std::numeric_limits<double>::quiet_NaN();
    }
}

void JogController::processOverride(bool spindle, int counts) {
    const std::string& param = spindle ? cfg_.spindleOverrideParam
                                       : cfg_.feedOverrideParam;
    const std::optional<double> current = api_.getParam(param);
    if (!current) {
        // Never compute an override from a value that was not read.
        logWarn("override: GetParam(%s) failed; wheel input ignored",
                param.c_str());
        return;
    }
    double delta = counts * (cfg_.overrideStep / 100.0);
    double value = std::clamp(*current + delta, kOverrideMin, kOverrideMax);
    if (api_.setParam(param, value)) {
        logDebug("override: %s %+.2f%% -> %.1f%%", param.c_str(),
                 delta * 100.0, value * 100.0);
    } else {
        logWarn("override: SetParam(%s) failed", param.c_str());
    }
}

} // namespace mpgd
