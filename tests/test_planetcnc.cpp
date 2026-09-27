#include "test_framework.h"

#include "logic/ButtonHandler.h"
#include "logic/DisplayUpdater.h"
#include "logic/JogController.h"
#include "logic/SharedState.h"
#include "planetcnc/ITngApi.h"
#include "planetcnc/StateReader.h"

#include <limits>
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
    double motorA = 0.0, motorB = 0.0, motorC = 0.0;

    std::map<std::string, double> params;

    // --- Call recording ---
    struct JogCall { bool step; double x, y, z; };
    std::vector<JogCall> jogs;
    struct Jog9Call { bool step; double a, b, c; };
    std::vector<Jog9Call> jogs9;
    struct MoveAxisCall { double speed; int axis; double value; };
    std::vector<MoveAxisCall> moveAxes;
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
    std::optional<double> getParam(const std::string& name) override {
        getParams.push_back(name);
        auto it = params.find(name);
        if (it == params.end()) return std::nullopt;
        return it->second;
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
    double infoMotorPosition(int axis) override {
        switch (axis) {
            case 0: return motorX;
            case 1: return motorY;
            case 2: return motorZ;
            case 3: return motorA;
            case 4: return motorB;
            case 5: return motorC;
            default: return 0.0;
        }
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
    bool moveAxis(double speed, int axis, double value) override {
        moveAxes.push_back({speed, axis, value});
        return true;
    }
};

Config makeConfig() {
    Config cfg;
    cfg.buttons.clear();  // tests bind only the buttons they exercise
    return cfg;
}

void addButton(Config& cfg, const std::string& name, const std::string& action) {
    ButtonAction a;
    a.action = action;
    cfg.buttons.emplace_back(name, a);
}

// ---------------------------------------------------------------------------
// JogController
// ---------------------------------------------------------------------------

static void test_servo_velocity() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.jogSpeed = 1.0;  // 1:1 so the Jog value equals the velocity
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;
    state.pendant.jogCounts.store(2);  // error 2 -> vel 10

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 1u);
    CHECK(!api.jogs[0].step);
    CHECK_NEAR(api.jogs[0].x, 10.0, 1e-9);
    CHECK_NEAR(api.jogs[0].y, 0.0, 1e-9);
    CHECK_NEAR(api.jogs[0].z, 0.0, 1e-9);
    CHECK_EQ(api.jogs9.size(), 0u);
    CHECK_EQ(api.moveAxes.size(), 0u);
}

static void test_servo_velocity_capped() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.jogSpeed = 1.0;
    cfg.maxSpeed = 600.0;  // maxVel = 10 mm/s
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 10.0;
    state.pendant.jogCounts.store(1);  // error 10 -> vel 50 -> capped 10

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 1u);
    CHECK_NEAR(api.jogs[0].x, 10.0, 1e-9);
}

static void test_servo_negative_direction() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.jogSpeed = 1.0;
    api.motorY = 0.0;
    state.pendant.axisCode = xhc::kAxisY;
    state.stepSize = 1.0;
    state.pendant.jogCounts.store(-1);  // error -1 -> vel -5

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 1u);
    CHECK_NEAR(api.jogs[0].y, -5.0, 1e-9);
}

static void test_servo_a_axis() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.jogSpeed = 1.0;
    api.motorA = 0.0;
    state.pendant.axisCode = xhc::kAxisA;
    state.stepSize = 0.1;
    state.pendant.jogCounts.store(5);  // error 0.5 -> vel 2.5

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 0u);
    CHECK_EQ(api.jogs9.size(), 1u);
    CHECK(!api.jogs9[0].step);
    CHECK_NEAR(api.jogs9[0].a, 2.5, 1e-9);
}

static void test_servo_accumulates_target() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.jogSpeed = 1.0;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(1);
    jc.tick();  // error 1 -> vel 5
    state.pendant.jogCounts.store(2);
    jc.tick();  // error 3 -> vel 15

    CHECK_EQ(api.jogs.size(), 2u);
    CHECK_NEAR(api.jogs[0].x, 5.0, 1e-9);
    CHECK_NEAR(api.jogs[1].x, 15.0, 1e-9);
}

static void test_servo_hysteresis() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();  // error 5 -> engage
    CHECK_EQ(api.jogs.size(), 1u);

    api.motorX = 5.0;
    jc.tick();  // error 0 -> stop
    CHECK_EQ(api.jogStops, 1);

    api.motorX = 4.9;  // error 0.1 < start threshold 0.3 -> no re-engage
    jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);
    CHECK_EQ(api.jogStops, 1);
}

static void test_servo_no_drive_back_after_external_move() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();  // engage toward 5
    api.motorX = 5.0;
    jc.tick();  // reached -> stop
    CHECK_EQ(api.jogs.size(), 1u);
    CHECK_EQ(api.jogStops, 1);

    // Something else (Home, G-code, TNG GUI) moves the axis far away.
    api.motorX = -100.0;
    for (int i = 0; i < 5; ++i) jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);  // must not servo back to 5

    // Next wheel click jogs relative to the new position.
    state.pendant.jogCounts.store(1);
    jc.tick();
    CHECK_EQ(api.jogs.size(), 2u);
    CHECK(api.jogs[1].x > 0.0);
    CHECK(api.jogs[1].x < 6.0);  // error 1 (not 106)
}

static void test_servo_target_reset_by_external_motion() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 0.1;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(1);  // target 0.1, below start deadband
    jc.tick();
    CHECK_EQ(api.jogs.size(), 0u);

    // Machine busy with motion that is not ours: pending target dropped.
    state.machine.idle = false;
    api.motorX = 50.0;
    jc.tick();
    state.machine.idle = true;

    state.pendant.jogCounts.store(1);  // 50 + 0.1: still below deadband
    jc.tick();
    CHECK_EQ(api.jogs.size(), 0u);
}

static void test_servo_target_reset_after_estop() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);

    state.machine.estop = true;
    api.motorX = 2.0;
    jc.tick();
    CHECK_EQ(api.jogStops, 1);

    state.machine.estop = false;  // e-stop released
    for (int i = 0; i < 5; ++i) jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);  // does not resume toward the old target
}

static void test_servo_stops_on_position_read_failure() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);

    api.motorX = std::numeric_limits<double>::quiet_NaN();
    jc.tick();
    CHECK_EQ(api.jogStops, 1);

    api.motorX = 1.0;  // read recovers: stale target must not be resumed
    jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);
}

static void test_servo_stops_on_jog_failure() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();  // vel 25
    CHECK_EQ(api.jogs.size(), 1u);

    api.jogResult = false;
    api.motorX = 4.0;  // vel 5: re-issue fails
    jc.tick();
    CHECK_EQ(api.jogStops, 1);
}

static void test_servo_blocked_while_program_runs() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);

    state.machine.running = true;
    jc.tick();
    CHECK_EQ(api.jogStops, 1);
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);

    state.machine.running = false;
    state.machine.paused = true;
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);
}

static void test_override_allowed_while_program_runs() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    state.machine.running = true;
    state.machine.idle = false;
    state.pendant.axisCode = xhc::kAxisFeed;
    api.params[cfg.feedOverrideParam] = 1.0;
    state.pendant.jogCounts.store(1);

    JogController jc(api, state, cfg);
    jc.tick();
    CHECK_EQ(api.setParams.size(), 1u);
}

static void test_servo_jogspeed_division() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.jogSpeed = 12.0;  // default: Jog value = vel / 12
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;
    state.pendant.jogCounts.store(2);  // error 2 -> vel 10 -> jog value 10/12

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 1u);
    CHECK_NEAR(api.jogs[0].x, 10.0 / 12.0, 1e-9);
}

static void test_servo_no_reissue_similar_velocity() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(2);
    jc.tick();  // error 2 -> vel 10
    CHECK_EQ(api.jogs.size(), 1u);

    api.motorX = 0.1;  // error 1.9 -> vel 9.5, |9.5-10| < 1 -> no re-issue
    jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);

    api.motorX = 0.5;  // error 1.5 -> vel 7.5, |7.5-10| > 1 -> re-issue
    jc.tick();
    CHECK_EQ(api.jogs.size(), 2u);
}

static void test_servo_blocked_by_estop() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    state.pendant.axisCode = xhc::kAxisX;
    state.machine.estop = true;
    state.pendant.jogCounts.store(5);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 0u);
    CHECK_EQ(api.moveAxes.size(), 0u);
}

// The shared e-stop flag may be stale (it is refreshed at the display rate,
// review item 16): the jog tick asks the controller directly before moving.
static void test_servo_checks_live_estop() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    api.estop = true;                  // shared state still says no e-stop
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.jogs.size(), 0u);

    api.estop = false;
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.jogs.size(), 1u);

    api.estop = true;                  // e-stop while the servo is running
    jc.tick();
    CHECK_EQ(api.jogStops, 1);
}

static void test_servo_disabled_in_attach_mode() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    state.pendant.axisCode = xhc::kAxisX;
    state.jogEnabled = false;
    state.pendant.jogCounts.store(5);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.jogs.size(), 0u);
    CHECK_EQ(api.moveAxes.size(), 0u);
}

static void test_override_feed() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.overrideStep = 10.0;
    api.params["_ovrd_speedfeed"] = 1.0;
    state.pendant.axisCode = xhc::kAxisFeed;
    state.pendant.jogCounts.store(2);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.setParams.size(), 1u);
    CHECK_EQ(api.setParams[0].first, std::string("_ovrd_speedfeed"));
    CHECK_NEAR(api.setParams[0].second, 1.2, 1e-9);
}

static void test_override_spindle_clamped() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    cfg.overrideStep = 10.0;
    api.params["_ovrd_spindle"] = 2.0;
    state.pendant.axisCode = xhc::kAxisSpindle;
    state.pendant.jogCounts.store(30); // +300% -> clamp to 250%

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.setParams.size(), 1u);
    CHECK_EQ(api.setParams[0].first, std::string("_ovrd_spindle"));
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
    api.params["_ovrd_speedfeed"] = 1.0;
    ButtonHandler bh(api, state, cfg);

    CHECK(bh.onPress("macro_1"));
    CHECK_NEAR(api.params["_ovrd_speedfeed"], 1.2, 1e-9);
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

static void test_button_step_size_cycles() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    cfg.jogging.stepSizes = {0.001, 0.01, 0.1};
    cfg.jogging.defaultStepIndex = 1;
    addButton(cfg, "step", "step_size");
    ButtonHandler bh(api, state, cfg);

    // Constructor applies the configured default step.
    CHECK_EQ(state.stepSizeIndex, 1);
    CHECK_NEAR(state.stepSize, 0.01, 1e-12);

    CHECK(bh.onPress("step"));
    CHECK_NEAR(state.stepSize, 0.1, 1e-12);
    CHECK(bh.onPress("step"));
    CHECK_NEAR(state.stepSize, 0.001, 1e-12);  // wraps around
    CHECK(bh.onPress("step"));
    CHECK_NEAR(state.stepSize, 0.01, 1e-12);
}

static void test_button_step_size_drives_jog_distance() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    cfg.jogging.stepSizes = {0.01, 1.0};
    cfg.jogging.defaultStepIndex = 0;
    addButton(cfg, "step", "step_size");
    ButtonHandler bh(api, state, cfg);
    CHECK(bh.onPress("step"));  // -> 1.0 mm per click

    state.pendant.axisCode = xhc::kAxisX;
    state.pendant.jogCounts.store(2);
    api.motorX = 0.0;
    JogController jc(api, state, cfg.jogging);
    jc.tick();

    // 2 mm error -> the servo starts moving in +X.
    CHECK_EQ(api.jogs.size(), 1u);
    CHECK(api.jogs[0].x > 0.0);
}

static void test_button_override_without_reading() {
    MockTngApi api; SharedState state; Config cfg = makeConfig();
    ButtonAction a;
    a.action = "feed_override";
    a.delta = 20.0;
    cfg.buttons.emplace_back("macro_1", a);
    // `_ovrd_speedfeed` is not readable (e.g. TNG not initialized yet).
    ButtonHandler bh(api, state, cfg);

    CHECK(!bh.onPress("macro_1"));
    CHECK_EQ(api.setParams.size(), 0u);
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
    api.paused = true;
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
    CHECK(state.machine.paused);
    CHECK(state.machine.initialized);
    CHECK(!state.machine.estop);
}

static void test_state_reader_estop_blocks() {
    MockTngApi api;
    api.estop = true;
    SharedState state;
    StateReader sr(api, state);

    CHECK(sr.read());
    CHECK(state.machine.estop);
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
    api.params["_ovrd_speedfeed"] = 1.0;
    api.params["_ovrd_spindle"] = 0.5;
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_servo_velocity();
    test_servo_velocity_capped();
    test_servo_negative_direction();
    test_servo_a_axis();
    test_servo_accumulates_target();
    test_servo_jogspeed_division();
    test_servo_hysteresis();
    test_servo_no_reissue_similar_velocity();
    test_servo_blocked_by_estop();
    test_servo_checks_live_estop();
    test_servo_disabled_in_attach_mode();
    test_servo_no_drive_back_after_external_move();
    test_servo_target_reset_by_external_motion();
    test_servo_target_reset_after_estop();
    test_servo_stops_on_position_read_failure();
    test_servo_stops_on_jog_failure();
    test_servo_blocked_while_program_runs();
    test_override_allowed_while_program_runs();
    test_override_feed();
    test_override_spindle_clamped();

    test_button_estop();
    test_button_stop_start_pause();
    test_button_toggle_start_pause();
    test_button_named_command();
    test_button_feed_override();
    test_button_gcode();
    test_button_step_size_cycles();
    test_button_step_size_drives_jog_distance();
    test_button_override_without_reading();
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
