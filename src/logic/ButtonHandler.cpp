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
    if (!action) return false;
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
    switch (a.type) {
        case ActionType::EStop:
            return api_.estopToggle();
        case ActionType::Stop:
            return api_.stop();
        case ActionType::Start:
            return api_.start();
        case ActionType::Pause:
            return api_.pause(true);
        case ActionType::PauseToggle:
            return api_.pauseToggle();
        case ActionType::ToggleStartPause:
            if (api_.isPause()) return api_.start();
            if (api_.isIdle() && !api_.isRunning()) return api_.start();
            return api_.pause(true);
        case ActionType::HomeAll:
            return execNamedCommand("Machine.Home");
        case ActionType::SetWorkZero:
            return execNamedCommand("Machine.Work_Position.Offset.To_Zero");
        case ActionType::SetWorkZeroXY:
            return execNamedCommand("Machine.Work_Position.Axis_To_Zero.XY");
        case ActionType::SetWorkZeroZ:
            return execNamedCommand("Machine.Work_Position.Axis_To_Zero.Z");
        case ActionType::SpindleToggle:
            return execNamedCommand("Machine.Spindle");
        case ActionType::FloodToggle:
            return execNamedCommand("Machine.Flood");
        case ActionType::MistToggle:
            return execNamedCommand("Machine.Mist");
        case ActionType::FeedOverride:
        case ActionType::SpindleOverride: {
            const std::string& param = (a.type == ActionType::FeedOverride)
                                           ? cfg_.jogging.feedOverrideParam
                                           : cfg_.jogging.spindleOverrideParam;
            const std::optional<double> current = api_.getParam(param);
            if (!current) {
                logWarn("button: GetParam(%s) failed; override unchanged", param.c_str());
                return false;
            }
            double value = std::clamp(*current + a.delta / 100.0, kOverrideMin, kOverrideMax);
            return api_.setParam(param, value);
        }
        case ActionType::StepSize: {
            const auto& steps = cfg_.jogging.stepSizes;
            if (steps.empty()) return false;
            std::lock_guard<std::mutex> lk(state_.mutex);
            state_.stepSizeIndex = (state_.stepSizeIndex + 1) % static_cast<int>(steps.size());
            state_.stepSize = steps[static_cast<size_t>(state_.stepSizeIndex)];
            logInfo("jog step size: %.3f mm", state_.stepSize);
            return true;
        }
        case ActionType::Command:
            if (a.command.empty()) return false;
            return execNamedCommand(a.command);
        case ActionType::GCode: {
            if (a.command.empty()) return false;
            bool ok = api_.startCode(a.command);
            if (!ok) logWarn("button: StartCode('%s') failed", a.command.c_str());
            return ok;
        }
        case ActionType::Noop:
            return true;
        case ActionType::Unknown:
            break;
    }

    logWarn("button: unknown action '%s'", a.name.c_str());
    return false;
}

} // namespace mpgd
