#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mpgd {

// Single button -> action binding.
struct ButtonAction {
    std::string action;              // "estop", "stop", "home_all", ...
    std::string command;             // used by "command"/"gcode" actions
    double delta = 0.0;              // used by override actions (percent)
    double value = 0.0;              // generic numeric value
};

struct DeviceConfig {
    uint16_t vendorId = 0x10CE;
    std::vector<uint16_t> productIds{0xEB70, 0xEB71, 0xEB93};
    bool autoDetect = true;
    bool verifyChecksum = false;     // reference driver does not verify; opt-in
};

struct PlanetCncConfig {
    std::string profile;             // profile name for RunProfile ("" = default)
    std::string libPath;             // explicit lib path ("" = auto-detect)
    bool attach = false;             // connect to already-running external TNG
};

struct JoggingConfig {
    std::vector<double> stepSizes{0.001, 0.01, 0.1, 1.0};  // mm
    double maxSpeed = 1000.0;        // mm/min
    std::string mode = "step";       // "step" | "continuous"
    double overrideStep = 10.0;      // percent per wheel click for override
    std::string feedOverrideParam = "SpeedFeedOverride";
    std::string spindleOverrideParam = "SpeedSpindleOverride";
};

struct PollingConfig {
    int usbHz = 100;                 // poll rate for USB input
    int displayHz = 20;              // LCD refresh rate
    int reconnectMs = 2000;          // USB reconnect interval
    int buttonDebounceMs = 50;       // SAFE-04
    bool displayAlways = false;      // keep sending LCD data when axis=OFF
};

struct LoggingConfig {
    std::string level = "info";      // trace|debug|info|warn|error|critical|off
    std::string file;                // empty = console only
};

struct Config {
    DeviceConfig device;
    PlanetCncConfig planetcnc;
    JoggingConfig jogging;
    PollingConfig polling;
    LoggingConfig logging;
    // button name (canonical, see XhcProtocol.h) -> action
    std::vector<std::pair<std::string, ButtonAction>> buttons;
};

class ConfigManager {
public:
    // Loads the YAML file into `out`. Returns true on success.
    static bool load(const std::string& path, Config& out, std::string& error);

    // Loads defaults and overlays the optional file. Never fails: on a
    // missing/invalid file the defaults are returned and `error` is set.
    static Config loadOrDefault(const std::string& path, std::string& error);

    // Looks up the action bound to a canonical button name. Returns nullptr
    // when no action is configured.
    static const ButtonAction* findAction(const Config& cfg,
                                          const std::string& buttonName);
};

} // namespace mpgd
