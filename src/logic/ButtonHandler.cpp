#include "logic/ButtonHandler.h"

#include "planetcnc/ITngApi.h"
#include "utils/Logger.h"

#include <algorithm>

namespace mpgd {

ButtonHandler::ButtonHandler(ITngApi& api, SharedState& state, const Config& cfg)
    : api_(api), state_(state), cfg_(cfg) {
    const auto& steps = cfg_.jogging.stepSizes;
    if (!steps.empty()) {
        const int last = static_cast<int>(steps.size()) - 1;
        const int idx = std::clamp(cfg_.jogging.defaultStepIndex, 0, last);
        std::lock_guard<std::mutex> lk(state_.mutex);
        state_.stepSizeIndex = idx;
        state_.stepSize = steps[static_cast<size_t>(idx)];
    }
}

bool ButtonHandler::onPress(const std::string& buttonName) {
    const ButtonAction* action = ConfigManager::findAction(cfg_, buttonName);
    if (!action || action->action.empty()) return false;
    return dispatch(*action);
}

bool ButtonHandler::execNamedCommand(const std::string& commandName) {
    int id = api_.getCmdId(commandName);
    if (id < 0) {
        logWarn("button: unknown TNG command '%s'", commandName.c_str());
        return false;
    }
    bool ok = api_.cmdExec(id);
    if (!ok) logWarn("button: CmdExec('%s') failed", commandName.c_str());
    return ok;
}

bool ButtonHandler::dispatch(const ButtonAction& a) {
    const std::string& action = a.action;

    if (action == "estop") {
        return api_.estopToggle();
    }
    if (action == "stop") {
        return api_.stop();
    }
    if (action == "start") {
        return api_.start();
    }
    if (action == "pause") {
        return api_.pause(true);
    }
    if (action == "pause_toggle") {
        return api_.pauseToggle();
    }
    if (action == "toggle_start_pause") {
        if (api_.isPause()) return api_.start();
        if (api_.isIdle() && !api_.isRunning()) return api_.start();
        return api_.pause(true);
    }
    if (action == "home_all") {
        return execNamedCommand("Machine.Home");
    }
    if (action == "set_work_zero") {
        return execNamedCommand("Machine.Work_Position.Offset.To_Zero");
    }
    if (action == "set_work_zero_xy") {
        return execNamedCommand("Machine.Work_Position.Axis_To_Zero.XY");
    }
    if (action == "set_work_zero_z") {
        return execNamedCommand("Machine.Work_Position.Axis_To_Zero.Z");
    }
    if (action == "spindle_toggle") {
        return execNamedCommand("Machine.Spindle");
    }
    if (action == "flood_toggle") {
        return execNamedCommand("Machine.Flood");
    }
    if (action == "mist_toggle") {
        return execNamedCommand("Machine.Mist");
    }
    if (action == "feed_override" || action == "spindle_override") {
        const std::string& param = (action == "feed_override")
                                       ? cfg_.jogging.feedOverrideParam
                                       : cfg_.jogging.spindleOverrideParam;
        const std::optional<double> current = api_.getParam(param);
        if (!current) {
            logWarn("button: GetParam(%s) failed; override unchanged",
                    param.c_str());
            return false;
        }
        double value = std::clamp(*current + a.delta / 100.0, 0.0, 2.5);
        return api_.setParam(param, value);
    }
    if (action == "step_size") {
        const auto& steps = cfg_.jogging.stepSizes;
        if (steps.empty()) return false;
        std::lock_guard<std::mutex> lk(state_.mutex);
        state_.stepSizeIndex =
            (state_.stepSizeIndex + 1) % static_cast<int>(steps.size());
        state_.stepSize = steps[static_cast<size_t>(state_.stepSizeIndex)];
        logInfo("jog step size: %.3f mm", state_.stepSize);
        return true;
    }
    if (action == "command") {
        if (a.command.empty()) return false;
        return execNamedCommand(a.command);
    }
    if (action == "gcode") {
        if (a.command.empty()) return false;
        bool ok = api_.startCode(a.command);
        if (!ok) logWarn("button: StartCode('%s') failed", a.command.c_str());
        return ok;
    }
    if (action == "noop") {
        return true;
    }

    logWarn("button: unknown action '%s'", action.c_str());
    return false;
}

} // namespace mpgd
