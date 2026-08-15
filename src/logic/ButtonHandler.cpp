#include "logic/ButtonHandler.h"

#include "planetcnc/TngApi.h"
#include "utils/Logger.h"

#include <algorithm>

namespace mpgd {

ButtonHandler::ButtonHandler(TngApi& api, SharedState& state, const Config& cfg)
    : api_(api), state_(state), cfg_(cfg) {}

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
    if (action == "feed_override") {
        double current = api_.getParam(cfg_.jogging.feedOverrideParam);
        double value = std::clamp(current + a.delta / 100.0, 0.0, 2.5);
        return api_.setParam(cfg_.jogging.feedOverrideParam, value);
    }
    if (action == "spindle_override") {
        double current = api_.getParam(cfg_.jogging.spindleOverrideParam);
        double value = std::clamp(current + a.delta / 100.0, 0.0, 2.5);
        return api_.setParam(cfg_.jogging.spindleOverrideParam, value);
    }
    if (action == "toggle_jog_mode") {
        std::lock_guard<std::mutex> lk(state_.mutex);
        state_.jogMode = (state_.jogMode == JogMode::Step)
                             ? JogMode::Continuous
                             : JogMode::Step;
        logInfo("jog mode: %s",
                state_.jogMode == JogMode::Step ? "step" : "continuous");
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
