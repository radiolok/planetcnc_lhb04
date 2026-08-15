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
        servoActive_ = false;
        servoAxis_ = -1;
        lastVel_ = 0.0;
    }
}

void JogController::stopNow() {
    state_.pendant.jogCounts.store(0);
    stopServo();
    for (double& t : target_) t = std::numeric_limits<double>::quiet_NaN();
}

void JogController::tick() {
    const int counts = drainCounts();

    bool jogEnabled;
    bool estop;
    int axis;
    bool feedSel;
    bool spindleSel;
    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        jogEnabled = state_.jogEnabled;
        estop = state_.machine.estop;
        axis = state_.selectedAxis();
        feedSel = state_.feedOverrideSelected();
        spindleSel = state_.spindleOverrideSelected();
    }

    if (estop || !jogEnabled) {
        stopServo();
        return;
    }

    if (axis >= 0) {
        if (counts != 0) updateTarget(axis, counts);
        runServo(axis);
    } else {
        stopServo();
        if (feedSel && counts != 0) {
            processOverride(/*spindle=*/false, counts);
        } else if (spindleSel && counts != 0) {
            processOverride(/*spindle=*/true, counts);
        }
    }
}

void JogController::updateTarget(int axis, int counts) {
    if (axis < 0 || axis >= 6) return;

    double stepSize;
    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        stepSize = state_.stepSize;
    }
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

void JogController::runServo(int axis) {
    if (axis < 0 || axis >= 6) return;

    // Switching axes: stop the previous axis's servo immediately.
    if (servoActive_ && servoAxis_ != axis) stopServo();

    if (!std::isfinite(target_[axis])) return;

    const double current = api_.infoMotorPosition(axis);
    if (!std::isfinite(current)) return;

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
        logWarn("jog: velocity jog on axis %d failed", axis);
    }
}

void JogController::processOverride(bool spindle, int counts) {
    const std::string& param = spindle ? cfg_.spindleOverrideParam
                                       : cfg_.feedOverrideParam;
    double current = api_.getParam(param);
    double delta = counts * (cfg_.overrideStep / 100.0);
    double value = std::clamp(current + delta, kOverrideMin, kOverrideMax);
    if (api_.setParam(param, value)) {
        logDebug("override: %s %+.2f%% -> %.1f%%", param.c_str(),
                 delta * 100.0, value * 100.0);
    } else {
        logWarn("override: SetParam(%s) failed", param.c_str());
    }
}

} // namespace mpgd
