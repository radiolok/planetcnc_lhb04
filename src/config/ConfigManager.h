#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mpgd {

// Button actions. The YAML names are resolved once, when the config is
// built, so a button press dispatches on the enum.
enum class ActionType {
    Unknown,            // name not recognised (a config error)
    EStop,
    Stop,
    Start,
    Pause,
    PauseToggle,
    ToggleStartPause,
    HomeAll,
    SetWorkZero,
    SetWorkZeroXY,
    SetWorkZeroZ,
    SpindleToggle,
    FloodToggle,
    MistToggle,
    FeedOverride,
    SpindleOverride,
    StepSize,
    Command,
    GCode,
    Noop,
};

// Resolves a YAML action name ("estop", "home_all", ...). Returns
// ActionType::Unknown for any other name.
ActionType parseActionType(const std::string& name);

// Single button -> action binding.
struct ButtonAction {
    ActionType type = ActionType::Unknown;
    std::string name;                // action name as written in the config
    std::string command;             // used by "command"/"gcode" actions
    double delta = 0.0;              // used by override actions (percent)
};

// Builds a binding from its YAML action name, resolving `type`.
ButtonAction makeButtonAction(const std::string& name, const std::string& command = "",
                              double delta = 0.0);

struct DeviceConfig {
    uint16_t vendorId = 0x10CE;
    std::vector<uint16_t> productIds{0xEB70, 0xEB71, 0xEB93};
    bool verifyChecksum = false;     // reference driver does not verify; opt-in
};

struct PlanetCncConfig {
    std::string profile;             // profile name for RunProfile ("" = default)
    std::string libPath;             // explicit lib path ("" = auto-detect)
    bool attach = false;             // connect to already-running external TNG
};

struct JoggingConfig {
    std::vector<double> stepSizes{0.001, 0.01, 0.1, 1.0};  // mm
    // Index into stepSizes selected at start-up; the `step_size` button
    // action cycles through the list from there.
    int defaultStepIndex = 1;
    double maxSpeed = 1000.0;        // mm/min
    double overrideStep = 10.0;      // percent per wheel click for override
    std::string feedOverrideParam = "_ovrd_speedfeed";
    std::string spindleOverrideParam = "_ovrd_spindle";
    // The controller's jog speed (mm/s). PlanetCNC Jog() treats the velocity
    // argument as a MULTIPLIER of this speed (value * jogSpeed = mm/s, capped
    // at the controller max speed), so mpgd divides its desired mm/s by this
    // to produce the Jog value. Matches the _jog_speed controller setting
    // (validated on Mk3/4: _jog_speed = 12 -> value 0.5 jogged at 6 mm/s).
    double jogSpeed = 12.0;
    // Deceleration the jog servo plans its braking distance with (mm/s^2).
    // Keep it at or below the controller's _motion_maxdec, or the axis
    // cannot stop within the planned distance and overshoots.
    double maxDecel = 200.0;
};

// Valid range of the feed/spindle override parameters (1.0 = 100 %).
constexpr double kOverrideMin = 0.0;
constexpr double kOverrideMax = 2.5;

struct PollingConfig {
    int usbHz = 100;                 // poll rate for USB input
    int displayHz = 20;              // LCD refresh rate
    int jogHz = 100;                 // jog servo tick rate
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
    // button name (canonical, see XhcProtocol.h) -> action. Defaults to the
    // bindings shipped in config/mpgd.yaml; a `buttons` section in the file
    // replaces them entirely.
    std::vector<std::pair<std::string, ButtonAction>> buttons = defaultButtons();

    static std::vector<std::pair<std::string, ButtonAction>> defaultButtons();
};

class ConfigManager {
public:
    // Loads the YAML file into `out` and validates the result. Returns false
    // with every problem listed in `error` when the file cannot be read or
    // parsed, or when any value is invalid.
    static bool load(const std::string& path, Config& out, std::string& error);

    // Loads defaults and overlays the file. A missing file is not an error:
    // the defaults are kept and `missing` is set. Any other problem (parse
    // error, invalid value) returns false; the caller must not start.
    static bool loadOrDefault(const std::string& path, Config& out, std::string& error,
                              bool& missing);

    // Checks every value in `cfg`. Returns false with all problems listed
    // (one per line) in `error`.
    static bool validate(const Config& cfg, std::string& error);

    // True for the action names ButtonHandler understands.
    static bool isKnownAction(const std::string& action);

    // Looks up the action bound to a canonical button name. Returns nullptr
    // when no action is configured.
    static const ButtonAction* findAction(const Config& cfg, const std::string& buttonName);
};

} // namespace mpgd
