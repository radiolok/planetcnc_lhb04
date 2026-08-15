#include "test_framework.h"

#include "logic/ButtonHandler.h"
#include "logic/DisplayUpdater.h"
#include "logic/JogController.h"
#include "logic/SharedState.h"
#include "planetcnc/ITngApi.h"
#include "planetcnc/StateReader.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace mpgd;

namespace {

// Recording mock of the PlanetCNC TNG API. Each call is captured and the
// return values are fully configurable, so the jog / button / state-read /
// display logic can be verified without the vendor SDK.
class MockTngApi : public ITngApi {
public:
    // --- Configurable return values / state ---
    bool running = false;
    bool initialized = true;
    bool controllerReady = true;
    bool idle = true;
    bool estop = false;
    bool paused = false;

    bool jogResult = true;
    bool cmdExecResult = true;
    bool setParamResult = true;
    bool startCodeResult = true;
    int nextCmdId = 7;

    double speed = 0.0;
    double spindle = 0.0;
    unsigned jogPot = 0;
    bool workPosOk = true;
    double workX = 0.0, workY = 0.0, workZ = 0.0;
    bool motorPosOk = true;
    double motorX = 0.0, motorY = 0.0, motorZ = 0.0;

    std::map<std::string, double> params;

    // --- Call recording ---
    struct JogCall { bool step; double x, y, z; };
    std::vector<JogCall> jogs;
    struct Jog9Call { bool step; double a, b, c; };
    std::vector<Jog9Call> jogs9;
    int jogStops = 0;
    int estopToggles = 0;
    int stops = 0;
    int starts = 0;
    std::vector<bool> pauses;
    int pauseToggles = 0;
    std::vector<std::string> getCmdIds;
    std::vector<int> cmdExecs;
    std::vector<std::pair<std::string, double>> setParams;
    std::vector<std::string> getParams;
    std::vector<std::string> startCodes;

    // --- ITngApi implementation ---
    bool isRunning() override { return running; }
    bool isInitialized() override { return initialized; }
    bool isControllerReady() override { return controllerReady; }
    bool isIdle() override { return idle; }
    bool isEStop() override { return estop; }
    bool isPause() override { return paused; }

    bool estopToggle() override { ++estopToggles; return true; }
    bool stop() override { ++stops; return true; }
    bool start() override { ++starts; return true; }
    bool pause(bool on) override { pauses.push_back(on); return true; }
    bool pauseToggle() override { ++pauseToggles; return true; }

    int getCmdId(const std::string& name) override {
        getCmdIds.push_back(name);
        return nextCmdId;
    }
    bool cmdExec(int id) override {
        cmdExecs.push_back(id);
        return cmdExecResult;
    }

    bool setParam(const std::string& name, double value) override {
        setParams.emplace_back(name, value);
        params[name] = value;
        return setParamResult;
    }
    double getParam(const std::string& name) override {
        getParams.push_back(name);
        auto it = params.find(name);
        return it == params.end() ? 0.0 : it->second;
    }

    bool startCode(const std::string& gcode) override {
        startCodes.push_back(gcode);
        return startCodeResult;
    }

    double infoSpeed() override { return speed; }
    double infoSpindle() override { return spindle; }
    unsigned infoJogPot() override { return jogPot; }
    bool infoWorkPosition3(double& x, double& y, double& z) override {
        if (workPosOk) { x = workX; y = workY; z = workZ; }
        return workPosOk;
    }
    bool infoMotorPosition3(double& x, double& y, double& z) override {
        if (motorPosOk) { x = motorX; y = motorY; z = motorZ; }
        return motorPosOk;
    }

    bool jog(bool step, double x, double y, double z) override {
        jogs.push_back({step, x, y, z});
        return jogResult;
    }
    bool jog9(bool step, double x, double y, double z,
              double a, double b, double c,
              double u, double v, double w) override {
        (void)x; (void)y; (void)z; (void)u; (void)v; (void)w;
        jogs9.push_back({step, a, b, c});
        return jogResult;
    }
    bool jogStop() override { ++jogStops; return true; }
};

Config makeConfig() {
    return Config{};
}

void addButton(Config& cfg, const std::string& name, const std::string& action) {
    ButtonAction a;
    a.action = action;
    cfg.buttons.emplace_back(name, a);
}

// ---------------------------------------------------------------------------
// JogController
// ---------------------------------------------------------------------------

static void test_jog_step_x() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 0.01;
    state.jogMode = JogMode::Step;
    state.pendant.jogCounts.store(5);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 1u);
    CHECK(api.jogs[0].step);
    CHECK_NEAR(api.jogs[0].x, 0.05, 1e-9);
    CHECK_NEAR(api.jogs[0].y, 0.0, 1e-9);
    CHECK_NEAR(api.jogs[0].z, 0.0, 1e-9);
    CHECK_EQ(api.jogs9.size(), 0u);
}

static void test_jog_step_negative_y() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    state.pendant.axisCode = xhc::kAxisY;
    state.stepSize = 0.1;
    state.jogMode = JogMode::Step;
    state.pendant.jogCounts.store(-3);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 1u);
    CHECK_NEAR(api.jogs[0].y, -0.3, 1e-9);
}

static void test_jog_a_axis_uses_jog9() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    state.pendant.axisCode = xhc::kAxisA;
    state.stepSize = 0.01;
    state.jogMode = JogMode::Step;
    state.pendant.jogCounts.store(2);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 0u);
    CHECK_EQ(api.jogs9.size(), 1u);
    CHECK(api.jogs9[0].step);
    CHECK_NEAR(api.jogs9[0].a, 0.02, 1e-9);
}

static void test_jog_continuous_speed() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.maxSpeed = 1000.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.jogMode = JogMode::Continuous;
    state.pendant.jogCounts.store(3);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 1u);
    CHECK(!api.jogs[0].step);
    CHECK_NEAR(api.jogs[0].x, 1000.0, 1e-9);
}

static void test_jog_continuous_idle_stops() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    state.pendant.axisCode = xhc::kAxisX;
    state.jogMode = JogMode::Continuous;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(1);
    jc.tick();
    CHECK_EQ(api.jogStops, 0);

    jc.tick(); // no new counts -> continuous motion must stop
    CHECK_EQ(api.jogStops, 1);
}

static void test_jog_blocked_by_estop() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    state.pendant.axisCode = xhc::kAxisX;
    state.jogMode = JogMode::Step;
    state.machine.estop = true;
    state.pendant.jogCounts.store(5);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 0u);
    CHECK_EQ(api.jogs9.size(), 0u);
}

static void test_jog_disabled_in_attach_mode() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    state.pendant.axisCode = xhc::kAxisX;
    state.jogMode = JogMode::Step;
    state.jogEnabled = false;
    state.pendant.jogCounts.store(5);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 0u);
}

static void test_override_feed() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.overrideStep = 10.0;
    api.params["SpeedFeedOverride"] = 1.0;
    state.pendant.axisCode = xhc::kAxisFeed;
    state.pendant.jogCounts.store(2);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.setParams.size(), 1u);
    CHECK_EQ(api.setParams[0].first, std::string("SpeedFeedOverride"));
    CHECK_NEAR(api.setParams[0].second, 1.2, 1e-9);
}

static void test_override_spindle_clamped() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.overrideStep = 10.0;
    api.params["SpeedSpindleOverride"] = 2.0;
    state.pendant.axisCode = xhc::kAxisSpindle;
    state.pendant.jogCounts.store(30); // +300% -> clamp to 250%

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.setParams.size(), 1u);
    CHECK_EQ(api.setParams[0].first, std::string("SpeedSpindleOverride"));
    CHECK_NEAR(api.setParams[0].second, 2.5, 1e-9);
}

// ---------------------------------------------------------------------------
// ButtonHandler
// ---------------------------------------------------------------------------

static void test_button_estop() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    addButton(cfg, "reset", "estop");
    ButtonHandler bh(api, state, cfg);
    CHECK(bh.onPress("reset"));
    CHECK_EQ(api.estopToggles, 1);
}

static void test_button_stop_start_pause() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    addButton(cfg, "stop", "stop");
    addButton(cfg, "start", "start");
    addButton(cfg, "pause", "pause");
    addButton(cfg, "pause_toggle", "pause_toggle");
    ButtonHandler bh(api, state, cfg);

    CHECK(bh.onPress("stop"));
    CHECK(bh.onPress("start"));
    CHECK(bh.onPress("pause"));
    CHECK(bh.onPress("pause_toggle"));

    CHECK_EQ(api.stops, 1);
    CHECK_EQ(api.starts, 1);
    CHECK_EQ(api.pauses.size(), 1u);
    CHECK(api.pauses[0]);
    CHECK_EQ(api.pauseToggles, 1);
}

static void test_button_toggle_start_pause() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    addButton(cfg, "start_pause", "toggle_start_pause");
    ButtonHandler bh(api, state, cfg);

    // Paused -> start.
    api.paused = true;
    CHECK(bh.onPress("start_pause"));
    CHECK_EQ(api.starts, 1);
    CHECK_EQ(api.pauses.size(), 0u);

    // Idle and not running -> start.
    api.paused = false; api.idle = true; api.running = false;
    CHECK(bh.onPress("start_pause"));
    CHECK_EQ(api.starts, 2);
    CHECK_EQ(api.pauses.size(), 0u);

    // Otherwise -> pause(true).
    api.idle = false;
    CHECK(bh.onPress("start_pause"));
    CHECK_EQ(api.starts, 2);
    CHECK_EQ(api.pauses.size(), 1u);
    CHECK(api.pauses[0]);
}

static void test_button_named_command() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    addButton(cfg, "home", "home_all");
    api.nextCmdId = 11;
    ButtonHandler bh(api, state, cfg);

    CHECK(bh.onPress("home"));
    CHECK_EQ(api.getCmdIds.size(), 1u);
    CHECK_EQ(api.getCmdIds[0], std::string("Machine.Home"));
    CHECK_EQ(api.cmdExecs.size(), 1u);
    CHECK_EQ(api.cmdExecs[0], 11);
}

static void test_button_feed_override() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    ButtonAction a;
    a.action = "feed_override";
    a.delta = 20.0;
    cfg.buttons.emplace_back("macro_1", a);
    api.params["SpeedFeedOverride"] = 1.0;
    ButtonHandler bh(api, state, cfg);

    CHECK(bh.onPress("macro_1"));
    CHECK_NEAR(api.params["SpeedFeedOverride"], 1.2, 1e-9);
}

static void test_button_gcode() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    ButtonAction a;
    a.action = "gcode";
    a.command = "G0 X10";
    cfg.buttons.emplace_back("macro_2", a);
    ButtonHandler bh(api, state, cfg);

    CHECK(bh.onPress("macro_2"));
    CHECK_EQ(api.startCodes.size(), 1u);
    CHECK_EQ(api.startCodes[0], std::string("G0 X10"));
}

static void test_button_toggle_jog_mode() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    addButton(cfg, "mode", "toggle_jog_mode");
    ButtonHandler bh(api, state, cfg);

    state.jogMode = JogMode::Step;
    CHECK(bh.onPress("mode"));
    CHECK(state.jogMode == JogMode::Continuous);
    CHECK(bh.onPress("mode"));
    CHECK(state.jogMode == JogMode::Step);
}

static void test_button_noop_and_unbound() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    addButton(cfg, "macro_3", "noop");
    ButtonHandler bh(api, state, cfg);

    CHECK(bh.onPress("macro_3"));
    CHECK(!bh.onPress("does_not_exist"));
}

static void test_button_unknown_action() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    addButton(cfg, "macro_4", "not_a_real_action");
    ButtonHandler bh(api, state, cfg);
    CHECK(!bh.onPress("macro_4"));
}

// ---------------------------------------------------------------------------
// StateReader
// ---------------------------------------------------------------------------

static void test_state_reader_not_initialized() {
    MockTngApi api;
    api.initialized = false;
    SharedState state;
    StateReader sr(api, state);

    CHECK(!sr.read());
    CHECK(!state.machine.initialized);
}

static void test_state_reader_reads_all_fields() {
    MockTngApi api;
    api.workX = 1.0; api.workY = 2.0; api.workZ = 3.0;
    api.motorX = 4.0; api.motorY = 5.0; api.motorZ = 6.0;
    api.speed = 10.0;
    api.spindle = 200.0;
    api.jogPot = 0x12;
    api.controllerReady = true;
    api.idle = false;
    api.running = true;
    SharedState state;
    StateReader sr(api, state);

    CHECK(sr.read());
    CHECK_NEAR(state.machine.workX, 1.0, 1e-9);
    CHECK_NEAR(state.machine.workY, 2.0, 1e-9);
    CHECK_NEAR(state.machine.workZ, 3.0, 1e-9);
    CHECK_NEAR(state.machine.motorX, 4.0, 1e-9);
    CHECK_NEAR(state.machine.motorZ, 6.0, 1e-9);
    CHECK_NEAR(state.machine.feed, 10.0, 1e-9);
    CHECK_NEAR(state.machine.spindle, 200.0, 1e-9);
    CHECK_EQ(state.machine.jogPot, 0x12u);
    CHECK(state.machine.controllerReady);
    CHECK(!state.machine.idle);
    CHECK(state.machine.running);
    CHECK(state.machine.initialized);
    CHECK(!state.estopBlocked);
}

static void test_state_reader_estop_blocks() {
    MockTngApi api;
    api.estop = true;
    SharedState state;
    StateReader sr(api, state);

    CHECK(sr.read());
    CHECK(state.machine.estop);
    CHECK(state.estopBlocked);
}

// ---------------------------------------------------------------------------
// DisplayUpdater
// ---------------------------------------------------------------------------

static uint8_t payloadByte(const DisplayUpdater::Frame& f, size_t off) {
    return f[off / 7][off % 7 + 1];
}

static uint16_t readLE16At(const DisplayUpdater::Frame& f, size_t off) {
    return static_cast<uint16_t>(payloadByte(f, off)) |
           (static_cast<uint16_t>(payloadByte(f, off + 1)) << 8);
}

static void test_display_skip_when_axis_off() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    state.pendant.axisCode = xhc::kAxisOff;
    DisplayUpdater du(api, state, cfg);
    DisplayUpdater::Frame f;
    CHECK(!du.build(f));
}

static void test_display_always_sends_when_axis_off() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    cfg.polling.displayAlways = true;
    state.pendant.axisCode = xhc::kAxisOff;
    DisplayUpdater du(api, state, cfg);
    DisplayUpdater::Frame f;
    CHECK(du.build(f));
}

static void test_display_frame_encoding() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    api.params["SpeedFeedOverride"] = 1.0;
    api.params["SpeedSpindleOverride"] = 0.5;
    api.speed = 50.0;
    api.spindle = 200.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 0.01;
    state.machine.workX = 12.345;
    state.machine.workY = -0.001;
    state.machine.workZ = 0.0;

    DisplayUpdater du(api, state, cfg);
    DisplayUpdater::Frame f;
    CHECK(du.build(f));

    for (const auto& r : f) {
        CHECK_EQ(r[0], xhc::kOutputReportId);
    }
    CHECK_EQ(payloadByte(f, 0), 0xFE);
    CHECK_EQ(payloadByte(f, 1), 0xFD);
    CHECK_EQ(payloadByte(f, 2), 0x0C);

    // Line 1 (work X): +12.345.
    CHECK_EQ(readLE16At(f, 3), 12);
    CHECK_EQ(readLE16At(f, 5), 3450);

    // Feed override x100 at 27, spindle override x100 at 29.
    CHECK_EQ(readLE16At(f, 27), 100);
    CHECK_EQ(readLE16At(f, 29), 50);
    // Feed value x60 at 31, spindle rps x60 at 33.
    CHECK_EQ(readLE16At(f, 31), 3000);
    CHECK_EQ(readLE16At(f, 33), 12000);

    // Stepsize code (0.01 mm -> 10 -> 0x03).
    CHECK_EQ(payloadByte(f, xhc::kDispStepsizeByte), xhc::kStepDisplay10);
}

} // namespace

int main() {
    test_jog_step_x();
    test_jog_step_negative_y();
    test_jog_a_axis_uses_jog9();
    test_jog_continuous_speed();
    test_jog_continuous_idle_stops();
    test_jog_blocked_by_estop();
    test_jog_disabled_in_attach_mode();
    test_override_feed();
    test_override_spindle_clamped();

    test_button_estop();
    test_button_stop_start_pause();
    test_button_toggle_start_pause();
    test_button_named_command();
    test_button_feed_override();
    test_button_gcode();
    test_button_toggle_jog_mode();
    test_button_noop_and_unbound();
    test_button_unknown_action();

    test_state_reader_not_initialized();
    test_state_reader_reads_all_fields();
    test_state_reader_estop_blocks();

    test_display_skip_when_axis_off();
    test_display_always_sends_when_axis_off();
    test_display_frame_encoding();

    return tfw::summary("test_planetcnc");
}
