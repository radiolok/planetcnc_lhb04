#include "config/ConfigManager.h"

#include "usb/XhcProtocol.h"

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace mpgd {

namespace {

struct ActionName {
    const char* name;
    ActionType type;
};

constexpr ActionName kActionNames[] = {
    {"estop", ActionType::EStop},
    {"stop", ActionType::Stop},
    {"start", ActionType::Start},
    {"pause", ActionType::Pause},
    {"pause_toggle", ActionType::PauseToggle},
    {"toggle_start_pause", ActionType::ToggleStartPause},
    {"home_all", ActionType::HomeAll},
    {"set_work_zero", ActionType::SetWorkZero},
    {"set_work_zero_xy", ActionType::SetWorkZeroXY},
    {"set_work_zero_z", ActionType::SetWorkZeroZ},
    {"spindle_toggle", ActionType::SpindleToggle},
    {"flood_toggle", ActionType::FloodToggle},
    {"mist_toggle", ActionType::MistToggle},
    {"feed_override", ActionType::FeedOverride},
    {"spindle_override", ActionType::SpindleOverride},
    {"step_size", ActionType::StepSize},
    {"command", ActionType::Command},
    {"gcode", ActionType::GCode},
    {"noop", ActionType::Noop},
};

constexpr const char* kLogLevels[] = {
    "trace", "debug", "info", "warn", "error", "critical", "off",
};

bool isKnownButton(const std::string& name) {
    if (name == "none") return false;  // buttonName() of an unknown code
    for (int code = 1; code <= 0xFF; ++code) {
        if (name == xhc::buttonName(static_cast<uint8_t>(code))) return true;
    }
    return false;
}

void applyLogging(const YAML::Node& n, LoggingConfig& c) {
    if (!n) return;
    if (n["level"]) c.level = n["level"].as<std::string>(c.level);
    if (n["file"]) c.file = n["file"].as<std::string>(c.file);
}

void applyPolling(const YAML::Node& n, PollingConfig& c) {
    if (!n) return;
    if (n["usb_hz"]) c.usbHz = n["usb_hz"].as<int>(c.usbHz);
    if (n["display_hz"]) c.displayHz = n["display_hz"].as<int>(c.displayHz);
    if (n["jog_hz"]) c.jogHz = n["jog_hz"].as<int>(c.jogHz);
    if (n["reconnect_ms"]) c.reconnectMs = n["reconnect_ms"].as<int>(c.reconnectMs);
    if (n["button_debounce_ms"]) c.buttonDebounceMs = n["button_debounce_ms"].as<int>(c.buttonDebounceMs);
    if (n["display_always"]) c.displayAlways = n["display_always"].as<bool>(c.displayAlways);
}

void applyDevice(const YAML::Node& n, DeviceConfig& c) {
    if (!n) return;
    if (n["vendor_id"]) c.vendorId = static_cast<uint16_t>(n["vendor_id"].as<unsigned>(c.vendorId));
    if (n["product_ids"]) {
        c.productIds.clear();
        for (const auto& pid : n["product_ids"]) {
            c.productIds.push_back(static_cast<uint16_t>(pid.as<unsigned>()));
        }
    }
    if (n["verify_checksum"]) c.verifyChecksum = n["verify_checksum"].as<bool>(c.verifyChecksum);
}

void applyPlanetCnc(const YAML::Node& n, PlanetCncConfig& c) {
    if (!n) return;
    if (n["profile"]) c.profile = n["profile"].as<std::string>(c.profile);
    if (n["lib_path"]) c.libPath = n["lib_path"].as<std::string>(c.libPath);
    if (n["attach"]) c.attach = n["attach"].as<bool>(c.attach);
}

void applyJogging(const YAML::Node& n, JoggingConfig& c) {
    if (!n) return;
    if (n["step_sizes"]) {
        c.stepSizes.clear();
        for (const auto& s : n["step_sizes"]) c.stepSizes.push_back(s.as<double>());
    }
    if (n["default_step_index"]) c.defaultStepIndex = n["default_step_index"].as<int>(c.defaultStepIndex);
    if (n["max_speed"]) c.maxSpeed = n["max_speed"].as<double>(c.maxSpeed);
    if (n["override_step"]) c.overrideStep = n["override_step"].as<double>(c.overrideStep);
    if (n["feed_override_param"]) c.feedOverrideParam = n["feed_override_param"].as<std::string>(c.feedOverrideParam);
    if (n["spindle_override_param"]) c.spindleOverrideParam = n["spindle_override_param"].as<std::string>(c.spindleOverrideParam);
    if (n["jog_speed"]) c.jogSpeed = n["jog_speed"].as<double>(c.jogSpeed);
    if (n["max_decel"]) c.maxDecel = n["max_decel"].as<double>(c.maxDecel);
}

void applyButtons(const YAML::Node& n, Config& c) {
    if (!n) return;
    // An explicit `buttons` section replaces the built-in bindings.
    c.buttons.clear();
    for (const auto& entry : n) {
        std::string name = entry.first.as<std::string>();
        const YAML::Node& b = entry.second;
        std::string action;
        std::string command;
        double delta = 0.0;
        if (b["action"]) action = b["action"].as<std::string>();
        if (b["cmd"]) command = b["cmd"].as<std::string>();
        else if (b["command"]) command = b["command"].as<std::string>();
        if (b["delta"]) delta = b["delta"].as<double>();
        c.buttons.emplace_back(std::move(name),
                               makeButtonAction(action, command, delta));
    }
}

} // namespace

ActionType parseActionType(const std::string& name) {
    for (const auto& a : kActionNames) {
        if (name == a.name) return a.type;
    }
    return ActionType::Unknown;
}

ButtonAction makeButtonAction(const std::string& name,
                              const std::string& command, double delta) {
    ButtonAction a;
    a.type = parseActionType(name);
    a.name = name;
    a.command = command;
    a.delta = delta;
    return a;
}

std::vector<std::pair<std::string, ButtonAction>> Config::defaultButtons() {
    auto bind = [](const char* action, const char* cmd = "") {
        return makeButtonAction(action, cmd);
    };
    // Keep in sync with config/mpgd.yaml.
    return {
        {"reset",       bind("estop")},
        {"stop",        bind("stop")},
        {"start_pause", bind("toggle_start_pause")},
        {"probe_z",     bind("command", "Machine.Work_Position.Measure_Height")},
        {"zero",        bind("set_work_zero")},
        {"home",        bind("home_all")},
        {"spindle",     bind("spindle_toggle")},
        {"step",        bind("step_size")},
    };
}

bool ConfigManager::isKnownAction(const std::string& action) {
    return parseActionType(action) != ActionType::Unknown;
}

bool ConfigManager::validate(const Config& cfg, std::string& error) {
    std::vector<std::string> problems;
    auto bad = [&](const std::string& msg) { problems.push_back(msg); };

    if (cfg.device.productIds.empty()) bad("device.product_ids is empty");

    const auto& j = cfg.jogging;
    if (j.stepSizes.empty()) bad("jogging.step_sizes is empty");
    for (double s : j.stepSizes) {
        if (!(s > 0.0)) {
            bad("jogging.step_sizes: every step must be > 0");
            break;
        }
    }
    if (j.defaultStepIndex < 0 ||
        j.defaultStepIndex >= static_cast<int>(j.stepSizes.size())) {
        bad("jogging.default_step_index is outside jogging.step_sizes");
    }
    if (!(j.maxSpeed > 0.0)) bad("jogging.max_speed must be > 0");
    if (!(j.jogSpeed > 0.0)) bad("jogging.jog_speed must be > 0");
    if (!(j.maxDecel > 0.0)) bad("jogging.max_decel must be > 0");
    if (!(j.overrideStep > 0.0)) bad("jogging.override_step must be > 0");
    if (j.feedOverrideParam.empty()) bad("jogging.feed_override_param is empty");
    if (j.spindleOverrideParam.empty()) bad("jogging.spindle_override_param is empty");

    // Poll periods are 1000 / hz ms; above 1000 Hz the period would be 0 ms
    // and the thread would spin at full CPU.
    const auto& p = cfg.polling;
    if (p.usbHz < 1 || p.usbHz > 1000) bad("polling.usb_hz must be 1..1000");
    if (p.displayHz < 1 || p.displayHz > 1000) bad("polling.display_hz must be 1..1000");
    if (p.jogHz < 1 || p.jogHz > 1000) bad("polling.jog_hz must be 1..1000");
    if (p.reconnectMs <= 0) bad("polling.reconnect_ms must be > 0");
    if (p.buttonDebounceMs < 0) bad("polling.button_debounce_ms must be >= 0");

    bool levelOk = false;
    for (const char* l : kLogLevels) {
        if (cfg.logging.level == l) levelOk = true;
    }
    if (!levelOk) bad("logging.level '" + cfg.logging.level + "' is not a known level");

    for (const auto& [name, a] : cfg.buttons) {
        if (!isKnownButton(name)) bad("buttons: unknown button '" + name + "'");
        if (a.type == ActionType::Unknown) {
            bad("buttons." + name + ": unknown action '" + a.name + "'");
        } else if ((a.type == ActionType::Command ||
                    a.type == ActionType::GCode) &&
                   a.command.empty()) {
            bad("buttons." + name + ": action '" + a.name + "' needs `cmd`");
        }
    }

    if (problems.empty()) return true;
    error.clear();
    for (const auto& msg : problems) {
        if (!error.empty()) error += "\n";
        error += msg;
    }
    return false;
}

bool ConfigManager::load(const std::string& path, Config& out, std::string& error) {
    try {
        YAML::Node root = YAML::LoadFile(path);
        if (!root.IsMap()) {
            error = "config root is not a map: " + path;
            return false;
        }

        applyDevice(root["device"], out.device);
        applyPlanetCnc(root["planetcnc"], out.planetcnc);
        applyJogging(root["jogging"], out.jogging);
        applyPolling(root["polling"], out.polling);
        applyLogging(root["logging"], out.logging);
        applyButtons(root["buttons"], out);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return validate(out, error);
}

bool ConfigManager::loadOrDefault(const std::string& path, Config& out,
                                  std::string& error, bool& missing) {
    out = Config{};
    missing = false;
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
        missing = true;
        return true;
    }
    return load(path, out, error);
}

const ButtonAction* ConfigManager::findAction(const Config& cfg,
                                              const std::string& buttonName) {
    for (const auto& [name, action] : cfg.buttons) {
        if (name == buttonName) return &action;
    }
    return nullptr;
}

} // namespace mpgd
