#include "test_framework.h"

#include "config/ConfigManager.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace mpgd;

namespace {

std::string writeTemp(const std::string& name, const std::string& text) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path) << text;
    return path.string();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void test_shipped_config_is_valid() {
    Config cfg;
    std::string err;
    CHECK(ConfigManager::load(MPGD_SOURCE_DIR "/config/mpgd.yaml", cfg, err));
    if (!err.empty()) std::printf("  error: %s\n", err.c_str());
    const ButtonAction* reset = ConfigManager::findAction(cfg, "reset");
    CHECK(reset && reset->type == ActionType::EStop);
    const ButtonAction* step = ConfigManager::findAction(cfg, "step");
    CHECK(step && step->type == ActionType::StepSize);
}

void test_defaults_are_valid_and_bind_estop() {
    Config cfg;
    std::string err;
    CHECK(ConfigManager::validate(cfg, err));
    const ButtonAction* reset = ConfigManager::findAction(cfg, "reset");
    CHECK(reset && reset->type == ActionType::EStop);
    const ButtonAction* stop = ConfigManager::findAction(cfg, "stop");
    CHECK(stop && stop->type == ActionType::Stop);
}

void test_missing_file_uses_defaults() {
    Config cfg;
    std::string err;
    bool missing = false;
    CHECK(ConfigManager::loadOrDefault("/nonexistent/mpgd.yaml", cfg, err, missing));
    CHECK(missing);
    CHECK(ConfigManager::findAction(cfg, "reset") != nullptr);
}

void test_parse_error_is_fatal() {
    auto path = writeTemp("mpgd_test_bad.yaml", "polling: { usb_hz: [\n");
    Config cfg;
    std::string err;
    bool missing = false;
    CHECK(!ConfigManager::loadOrDefault(path, cfg, err, missing));
    CHECK(!missing);
    CHECK(!err.empty());
    std::filesystem::remove(path);
}

void test_invalid_values_all_reported() {
    auto path = writeTemp("mpgd_test_invalid.yaml",
                          "polling:\n"
                          "  usb_hz: 2000\n"
                          "  display_hz: 0\n"
                          "  jog_hz: 5000\n"
                          "jogging:\n"
                          "  jog_speed: -1\n"
                          "  max_decel: 0\n"
                          "  max_speed: 0\n"
                          "  step_sizes: [0.01, 0]\n"
                          "logging:\n"
                          "  level: verbose\n"
                          "buttons:\n"
                          "  resett: { action: estop }\n"
                          "  stop:   { action: stahp }\n"
                          "  home:   { action: command }\n");
    Config cfg;
    std::string err;
    CHECK(!ConfigManager::load(path, cfg, err));
    CHECK(contains(err, "usb_hz"));
    CHECK(contains(err, "display_hz"));
    CHECK(contains(err, "jog_hz"));
    CHECK(contains(err, "jog_speed"));
    CHECK(contains(err, "max_decel"));
    CHECK(contains(err, "max_speed"));
    CHECK(contains(err, "step_sizes"));
    CHECK(contains(err, "logging.level"));
    CHECK(contains(err, "unknown button 'resett'"));
    CHECK(contains(err, "unknown action 'stahp'"));
    CHECK(contains(err, "needs `cmd`"));
    std::filesystem::remove(path);
}

void test_buttons_section_replaces_defaults() {
    auto path = writeTemp("mpgd_test_buttons.yaml",
                          "buttons:\n"
                          "  macro_1: { action: noop }\n");
    Config cfg;
    std::string err;
    CHECK(ConfigManager::load(path, cfg, err));
    CHECK_EQ(cfg.buttons.size(), 1u);
    CHECK(ConfigManager::findAction(cfg, "reset") == nullptr);
    std::filesystem::remove(path);
}

void test_no_buttons_section_keeps_defaults() {
    auto path = writeTemp("mpgd_test_nobuttons.yaml",
                          "polling:\n"
                          "  usb_hz: 50\n");
    Config cfg;
    std::string err;
    CHECK(ConfigManager::load(path, cfg, err));
    CHECK_EQ(cfg.polling.usbHz, 50);
    CHECK(ConfigManager::findAction(cfg, "reset") != nullptr);
    std::filesystem::remove(path);
}

void test_default_step_index_out_of_range() {
    Config cfg;
    cfg.jogging.stepSizes = {0.1};
    cfg.jogging.defaultStepIndex = 1;
    std::string err;
    CHECK(!ConfigManager::validate(cfg, err));
    CHECK(contains(err, "default_step_index"));
}

void test_action_names_resolve_to_types() {
    CHECK(parseActionType("estop") == ActionType::EStop);
    CHECK(parseActionType("home_all") == ActionType::HomeAll);
    CHECK(parseActionType("gcode") == ActionType::GCode);
    CHECK(parseActionType("stahp") == ActionType::Unknown);
    CHECK(parseActionType("") == ActionType::Unknown);

    const ButtonAction a = makeButtonAction("spindle_override", "", -10.0);
    CHECK(a.type == ActionType::SpindleOverride);
    CHECK_EQ(a.name, std::string("spindle_override"));
    CHECK_NEAR(a.delta, -10.0, 1e-12);
}

void test_partial_sections_keep_other_defaults() {
    auto path = writeTemp("mpgd_test_partial.yaml",
                          "jogging:\n"
                          "  max_speed: 1500\n"
                          "polling:\n"
                          "  display_hz: 10\n");
    Config cfg;
    std::string err;
    CHECK(ConfigManager::load(path, cfg, err));
    const Config defaults;
    CHECK_NEAR(cfg.jogging.maxSpeed, 1500.0, 1e-9);
    CHECK_NEAR(cfg.jogging.jogSpeed, defaults.jogging.jogSpeed, 1e-9);
    CHECK_EQ(cfg.jogging.stepSizes.size(), defaults.jogging.stepSizes.size());
    CHECK_EQ(cfg.polling.displayHz, 10);
    CHECK_EQ(cfg.polling.usbHz, defaults.polling.usbHz);
    CHECK_EQ(cfg.device.vendorId, defaults.device.vendorId);
    CHECK_EQ(cfg.logging.level, defaults.logging.level);
    std::filesystem::remove(path);
}

void test_empty_file_is_rejected() {
    auto path = writeTemp("mpgd_test_empty.yaml", "");
    Config cfg;
    std::string err;
    bool missing = false;
    const bool ok = ConfigManager::loadOrDefault(path, cfg, err, missing);
    // An empty document is not a mapping: reject it rather than guess.
    CHECK(!ok);
    CHECK(!err.empty());
    std::filesystem::remove(path);
}

// A value of the wrong type must fail the load, not silently keep the
// default (a typo in max_speed would otherwise go unnoticed).
void test_wrong_value_type_is_fatal() {
    const char* cases[] = {
        "polling:\n  usb_hz: fast\n",
        "jogging:\n  max_speed: [1, 2]\n",
        "device:\n  verify_checksum: maybe\n",
        "jogging:\n  step_sizes: [0.1, abc]\n",
    };
    for (const char* text : cases) {
        auto path = writeTemp("mpgd_test_type.yaml", text);
        Config cfg;
        std::string err;
        bool missing = false;
        const bool ok = ConfigManager::loadOrDefault(path, cfg, err, missing);
        CHECK(!ok);
        CHECK(!err.empty());
        if (ok) std::printf("  accepted: %s\n", text);
        std::filesystem::remove(path);
    }
}

void test_section_of_wrong_shape_is_fatal() {
    auto path = writeTemp("mpgd_test_shape.yaml", "polling: 100\n");
    Config cfg;
    std::string err;
    bool missing = false;
    CHECK(!ConfigManager::loadOrDefault(path, cfg, err, missing));
    CHECK(!err.empty());
    std::filesystem::remove(path);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_shipped_config_is_valid();
    test_defaults_are_valid_and_bind_estop();
    test_missing_file_uses_defaults();
    test_parse_error_is_fatal();
    test_invalid_values_all_reported();
    test_buttons_section_replaces_defaults();
    test_no_buttons_section_keeps_defaults();
    test_default_step_index_out_of_range();
    test_action_names_resolve_to_types();
    test_partial_sections_keep_other_defaults();
    test_empty_file_is_rejected();
    test_wrong_value_type_is_fatal();
    test_section_of_wrong_shape_is_fatal();
    return tfw::summary("test_config");
}
