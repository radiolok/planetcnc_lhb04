#include "config/ConfigManager.h"

#include <yaml-cpp/yaml.h>

#include <sstream>
#include <stdexcept>

namespace mpgd {

namespace {

uint16_t parseUint16(const std::string& s) {
    unsigned long v = std::stoul(s, nullptr, 0);
    return static_cast<uint16_t>(v);
}

std::string scalarToString(const YAML::Node& n, const std::string& fallback) {
    if (!n) return fallback;
    return n.as<std::string>(fallback);
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
    if (n["auto_detect"]) c.autoDetect = n["auto_detect"].as<bool>(c.autoDetect);
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
    if (n["max_speed"]) c.maxSpeed = n["max_speed"].as<double>(c.maxSpeed);
    if (n["mode"]) c.mode = n["mode"].as<std::string>(c.mode);
    if (n["override_step"]) c.overrideStep = n["override_step"].as<double>(c.overrideStep);
    if (n["feed_override_param"]) c.feedOverrideParam = n["feed_override_param"].as<std::string>(c.feedOverrideParam);
    if (n["spindle_override_param"]) c.spindleOverrideParam = n["spindle_override_param"].as<std::string>(c.spindleOverrideParam);
    if (n["jog_speed"]) c.jogSpeed = n["jog_speed"].as<double>(c.jogSpeed);
}

void applyButtons(const YAML::Node& n, Config& c) {
    if (!n) return;
    for (const auto& entry : n) {
        std::string name = entry.first.as<std::string>();
        const YAML::Node& b = entry.second;
        ButtonAction a;
        if (b["action"]) a.action = b["action"].as<std::string>();
        if (b["cmd"]) a.command = b["cmd"].as<std::string>();
        else if (b["command"]) a.command = b["command"].as<std::string>();
        if (b["delta"]) a.delta = b["delta"].as<double>();
        if (b["value"]) a.value = b["value"].as<double>();
        c.buttons.emplace_back(std::move(name), std::move(a));
    }
}

} // namespace

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
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

Config ConfigManager::loadOrDefault(const std::string& path, std::string& error) {
    Config cfg;
    if (path.empty()) return cfg;
    if (!load(path, cfg, error)) {
        // Keep the error for logging, but fall back to defaults so the daemon
        // can still start.
        return Config{};
    }
    return cfg;
}

const ButtonAction* ConfigManager::findAction(const Config& cfg,
                                              const std::string& buttonName) {
    for (const auto& [name, action] : cfg.buttons) {
        if (name == buttonName) return &action;
    }
    return nullptr;
}

} // namespace mpgd
