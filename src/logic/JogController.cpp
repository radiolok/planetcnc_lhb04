#include "logic/JogController.h"

#include "logic/JogMath.h"
#include "planetcnc/TngApi.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>

namespace mpgd {

namespace {
constexpr double kOverrideMin = 0.0;
constexpr double kOverrideMax = 2.5;
} // namespace

JogController::JogController(TngApi& api, SharedState& state,
                             const JoggingConfig& cfg)
    : api_(api), state_(state), cfg_(cfg) {}

int JogController::drainCounts() {
    int counts = state_.pendant.jogCounts.exchange(0);
    int clamped = clampAccumulatedCounts(counts);
    if (clamped != counts) {
        logWarn("jog: accumulated counts %d exceeded clamp bound", counts);
    }
    return clamped;
}

void JogController::ensureStopped(bool& wasStopped) {
    if (!wasStopped) {
        api_.jogStop();
        wasStopped = true;
    }
}

void JogController::stopNow() {
    state_.pendant.jogCounts.store(0);
    api_.jogStop();
    stopped_ = true;
}

void JogController::tick() {
    int counts = drainCounts();
    if (counts == 0) {
        // In continuous mode, wheel idle means motion should stop.
        if (!stopped_ && state_.jogMode == JogMode::Continuous) {
            api_.jogStop();
            stopped_ = true;
        }
        return;
    }

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

    if (estop) {
        logWarn("jog: blocked by e-stop");
        ensureStopped(stopped_);
        return;
    }

    if (!jogEnabled) {
        logDebug("jog: disabled (attach mode), ignoring %d counts", counts);
        return;
    }

    if (axis >= 0) {
        processJog(axis, counts);
    } else if (feedSel) {
        processOverride(/*spindle=*/false, counts);
    } else if (spindleSel) {
        processOverride(/*spindle=*/true, counts);
    } else {
        // Rotary in OFF position: no axis and no override target.
        ensureStopped(stopped_);
    }
}

void JogController::processJog(int axis, int counts) {
    double stepSize;
    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        stepSize = state_.stepSize;
    }
    if (stepSize <= 0.0) stepSize = 0.001;

    const double sign = (counts < 0) ? -1.0 : 1.0;

    // Axis vector: X, Y, Z, A, B, C (index 0..5).
    double v[6] = {0, 0, 0, 0, 0, 0};
    if (axis >= 0 && axis < 6) {
        if (state_.jogMode == JogMode::Step) {
            v[axis] = jogDistanceMm(counts, stepSize);
        } else {
            // Continuous mode: speed bounded by cfg_.maxSpeed; the exact
            // interpretation of Jog(speed) is validated on real hardware.
            v[axis] = sign * std::max(1.0, cfg_.maxSpeed);
        }
    }

    const bool step = (state_.jogMode == JogMode::Step);
    bool ok;
    if (axis < 3) {
        ok = api_.jog(step, v[0], v[1], v[2]);
    } else {
        // A/B/C axes require the 9-axis variant.
        ok = api_.jog9(step, v[0], v[1], v[2], v[3], v[4], v[5], 0, 0, 0);
    }

    if (ok) {
        stopped_ = false;
    } else {
        logWarn("jog: Jog%s failed", step ? "(step)" : "(continuous)");
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
    ensureStopped(stopped_);
}

} // namespace mpgd
