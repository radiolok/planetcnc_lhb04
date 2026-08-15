#pragma once

#include "config/ConfigManager.h"
#include "logic/SharedState.h"

#include <cstdint>
#include <map>
#include <string>

namespace mpgd {

class ITngApi;

// Dispatches button presses to TNG commands per the YAML button mapping.
// Edge detection and debounce live in the USB poll thread; this class only
// executes the bound action for a canonical button name.
class ButtonHandler {
public:
    ButtonHandler(ITngApi& api, SharedState& state, const Config& cfg);

    // Executes the action bound to `buttonName`. Returns false when no action
    // is bound or the action is unknown.
    bool onPress(const std::string& buttonName);

    // Resolves and executes a named PlanetCNC command (e.g. "Machine.Home").
    bool execNamedCommand(const std::string& commandName);

private:
    bool dispatch(const ButtonAction& action);

    ITngApi& api_;
    SharedState& state_;
    const Config& cfg_;
};

} // namespace mpgd
