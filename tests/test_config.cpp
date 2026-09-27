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
    CHECK(reset && reset->action == "estop");
    const ButtonAction* step = ConfigManager::findAction(cfg, "step");
    CHECK(step && step->action == "step_size");
}

void test_defaults_are_valid_and_bind_estop() {
    Config cfg;
    std::string err;
    CHECK(ConfigManager::validate(cfg, err));
    const ButtonAction* reset = ConfigManager::findAction(cfg, "reset");
    CHECK(reset && reset->action == "estop");
    const ButtonAction* stop = ConfigManager::findAction(cfg, "stop");
    CHECK(stop && stop->action == "stop");
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
        "jogging:\n"
        "  jog_speed: -1\n"
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
    CHECK(contains(err, "jog_speed"));
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
    return tfw::summary("test_config");
}
