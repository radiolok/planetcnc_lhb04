#include "test_framework.h"

#include "logic/ButtonHandler.h"
#include "logic/DisplayUpdater.h"
#include "logic/JogController.h"
#include "logic/SharedState.h"
#include "planetcnc/ITngApi.h"
#include "planetcnc/StateReader.h"

#include <chrono>
#include <limits>
#include <map>
#include <string>
#include <thread>
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
    // Program executing. Modelled like the real TNG: IsRunning (DLL export
    // IsRun) only reports that TNG itself is alive and is always true, while
    // IsStop is true exactly when no program/motion is executing.
    bool running = false;
    bool initialized = true;
    bool controllerReady = true;
    bool idle = true;
    bool estop = false;
    bool paused = false;

    bool jogResult = true;
    bool moveResult = true;
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
    double workA = 0.0;

    std::map<std::string, double> params;

    // --- Call recording ---
    struct JogCall {
        bool step;
        double x, y, z;
    };
    std::vector<JogCall> jogs;
    struct Jog9Call {
        bool step;
        double a, b, c;
    };
    std::vector<Jog9Call> jogs9;
    struct MoveAxisCall {
        double speed;
        int axis;
        double value;
    };
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
    bool isRunning() override { return true; }
    bool isInitialized() override { return initialized; }
    bool isControllerReady() override { return controllerReady; }
    bool isIdle() override { return idle; }
    bool isStop() override { return !running; }
    bool isEStop() override { return estop; }
    bool isPause() override { return paused; }

    bool estopToggle() override {
        ++estopToggles;
        return true;
    }
    bool stop() override {
        ++stops;
        return true;
    }
    bool start() override {
        ++starts;
        return true;
    }
    bool pause(bool on) override {
        pauses.push_back(on);
        return true;
    }
    bool pauseToggle() override {
        ++pauseToggles;
        return true;
    }

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
        if (workPosOk) {
            x = workX;
            y = workY;
            z = workZ;
        }
        return workPosOk;
    }
    bool infoMotorPosition3(double& x, double& y, double& z) override {
        if (motorPosOk) {
            x = motorX;
            y = motorY;
            z = motorZ;
        }
        return motorPosOk;
    }
    double infoMotorPosition(int axis) override {
        switch (axis) {
            case 0:
                return motorX;
            case 1:
                return motorY;
            case 2:
                return motorZ;
            case 3:
                return motorA;
            case 4:
                return motorB;
            case 5:
                return motorC;
            default:
                return 0.0;
        }
    }

    double infoWorkPosition(int axis) override {
        switch (axis) {
            case 0:
                return workX;
            case 1:
                return workY;
            case 2:
                return workZ;
            case 3:
                return workA;
            default:
                return std::numeric_limits<double>::quiet_NaN();
        }
    }

    bool jog(bool step, double x, double y, double z) override {
        jogs.push_back({step, x, y, z});
        return jogResult;
    }
    bool jog9(bool step, double x, double y, double z, double a, double b, double c, double u,
              double v, double w) override {
        (void)x;
        (void)y;
        (void)z;
        (void)u;
        (void)v;
        (void)w;
        jogs9.push_back({step, a, b, c});
        return jogResult;
    }
    bool jogStop() override {
        ++jogStops;
        return true;
    }
    bool moveAxis(double moveSpeed, int axis, double value) override {
        moveAxes.push_back({moveSpeed, axis, value});
        return moveResult;
    }
};

Config makeConfig() {
    Config cfg;
    cfg.buttons.clear();  // tests bind only the buttons they exercise
    return cfg;
}

void addButton(Config& cfg, const std::string& name, const std::string& action) {
    cfg.buttons.emplace_back(name, makeButtonAction(action));
}

// ---------------------------------------------------------------------------
// JogController
// ---------------------------------------------------------------------------

// Long enough for the jog controller's settle timeout (150 ms).
void waitSettle() { std::this_thread::sleep_for(std::chrono::milliseconds(160)); }

static void test_move_single_click() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;  // maxSpeed = 1000 mm/min -> MoveAxis speed = 10000
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;
    state.pendant.jogCounts.store(1);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.moveAxes.size(), 1u);
    CHECK_NEAR(api.moveAxes[0].speed, 10000.0, 1e-9);
    CHECK_EQ(api.moveAxes[0].axis, 0);
    CHECK_NEAR(api.moveAxes[0].value, 1.0, 1e-9);
    CHECK_EQ(api.jogs.size(), 0u);
    CHECK_EQ(api.jogs9.size(), 0u);
}

static void test_move_small_click_not_ignored() {
    // Regression: a single click at the smallest step (0.001 mm) must move,
    // instead of being swallowed by a start deadband.
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 10.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 0.001;
    state.pendant.jogCounts.store(1);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.moveAxes.size(), 1u);
    CHECK_NEAR(api.moveAxes[0].value, 10.001, 1e-9);
}

static void test_move_negative_direction() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorY = 0.0;
    state.pendant.axisCode = xhc::kAxisY;
    state.stepSize = 1.0;
    state.pendant.jogCounts.store(-1);

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.moveAxes.size(), 1u);
    CHECK_EQ(api.moveAxes[0].axis, 1);
    CHECK_NEAR(api.moveAxes[0].value, -1.0, 1e-9);
}

static void test_move_a_axis() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorA = 0.0;
    state.pendant.axisCode = xhc::kAxisA;
    state.stepSize = 0.1;
    state.pendant.jogCounts.store(5);  // 0.5 mm

    JogController jc(api, state, cfg);
    jc.tick();

    CHECK_EQ(api.moveAxes.size(), 1u);
    CHECK_EQ(api.moveAxes[0].axis, 3);
    CHECK_NEAR(api.moveAxes[0].value, 0.5, 1e-9);
}

static void test_move_stops_at_target_no_oscillation() {
    // Once arrived, no further MoveAxis is issued (no limit cycle).
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(1);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);

    api.motorX = 1.0;  // arrived
    jc.tick();
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);
}

static void test_move_coalesces_rapid_ticks() {
    // Target changes within the re-issue interval coalesce into a single
    // MoveAxis; after the interval elapses the latest target is re-issued.
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(1);
    jc.tick();  // move to 1.0
    CHECK_EQ(api.moveAxes.size(), 1u);

    state.pendant.jogCounts.store(1);
    jc.tick();  // target 2.0, within interval -> no new move
    CHECK_EQ(api.moveAxes.size(), 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    state.pendant.jogCounts.store(1);
    jc.tick();  // target 3.0, interval elapsed -> re-issue
    CHECK_EQ(api.moveAxes.size(), 2u);
    CHECK_NEAR(api.moveAxes[1].value, 3.0, 1e-9);
}

static void test_move_settles_to_new_anchor() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(1);
    jc.tick();
    api.motorX = 0.998;  // within motor resolution of the target
    waitSettle();
    jc.tick();  // idle + quiet -> settle on the commanded target

    state.pendant.jogCounts.store(1);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 2u);
    CHECK_NEAR(api.moveAxes[1].value, 2.0, 1e-9);

    api.motorX = 1.5;  // stopped well short (soft limit): adopt reached position
    waitSettle();
    jc.tick();
    state.pendant.jogCounts.store(1);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 3u);
    CHECK_NEAR(api.moveAxes[2].value, 2.5, 1e-9);
}

static void test_move_sub_resolution_steps_accumulate() {
    // Regression: steps finer than the motor resolution must not be lost when
    // the axis settles without having moved (anchor kept on the target).
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 39.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 0.001;

    JogController jc(api, state, cfg);
    for (int i = 0; i < 3; ++i) {
        state.pendant.jogCounts.store(1);
        jc.tick();
        waitSettle();
        jc.tick();  // motor still reports 39.0
    }
    CHECK_EQ(api.moveAxes.size(), 3u);
    CHECK_NEAR(api.moveAxes[2].value, 39.003, 1e-9);
}

static void test_move_input_not_dropped_at_settle() {
    // A coalesced target still pending when the move would settle is sent
    // instead of being discarded.
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(1);
    jc.tick();
    state.pendant.jogCounts.store(1);
    jc.tick();  // coalesced: target 2.0 not yet sent
    api.motorX = 1.0;
    waitSettle();
    jc.tick();  // must send the pending 2.0 instead of settling at 1.0
    CHECK_EQ(api.moveAxes.size(), 2u);
    CHECK_NEAR(api.moveAxes[1].value, 2.0, 1e-9);
}

static void test_move_no_drive_back_after_external_move() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();  // move toward 5
    api.motorX = 5.0;
    waitSettle();
    jc.tick();  // settled
    CHECK_EQ(api.moveAxes.size(), 1u);

    // Something else (Home, G-code, TNG GUI) moves the axis far away.
    state.machine.idle = false;
    api.motorX = -100.0;
    state.pendant.jogCounts.store(1);  // wheel input during foreign motion
    for (int i = 0; i < 5; ++i) jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);  // input discarded, no drive back

    // Next wheel click jogs relative to the new position.
    state.machine.idle = true;
    jc.tick();
    state.pendant.jogCounts.store(1);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 2u);
    CHECK_NEAR(api.moveAxes[1].value, -99.0, 1e-9);
}

static void test_move_reset_after_estop() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);

    state.machine.estop = true;
    api.motorX = 2.0;
    jc.tick();
    CHECK_EQ(api.jogStops, 1);

    state.machine.estop = false;  // e-stop released
    for (int i = 0; i < 5; ++i) jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);  // does not resume toward the old target

    state.pendant.jogCounts.store(1);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 2u);
    CHECK_NEAR(api.moveAxes[1].value, 3.0, 1e-9);
}

static void test_move_stops_on_axis_change() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);

    state.pendant.axisCode = xhc::kAxisY;
    jc.tick();
    CHECK_EQ(api.jogStops, 1);
    CHECK_EQ(api.moveAxes.size(), 1u);
}

static void test_move_needs_valid_position() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = std::numeric_limits<double>::quiet_NaN();
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(1);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 0u);

    api.motorX = 7.0;  // read recovers: jog from the recovered position
    state.pendant.jogCounts.store(1);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);
    CHECK_NEAR(api.moveAxes[0].value, 8.0, 1e-9);
}

static void test_move_stops_on_failure() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;
    api.moveResult = false;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);
    CHECK_EQ(api.jogStops, 1);

    for (int i = 0; i < 5; ++i) jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);  // no retry without new wheel input
}

static void test_move_rejected_reissue_retried() {
    // Regression: MoveAxis re-issued while a move is in flight may be
    // rejected by the controller. The accumulated offset must be kept and the
    // target retried, not dropped.
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(1);
    jc.tick();  // first move accepted (target 1.0)
    state.machine.idle = false;
    state.machine.running = true;  // TNG reports our own move as running

    api.moveResult = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    state.pendant.jogCounts.store(2);
    jc.tick();  // re-issue to 3.0 rejected
    CHECK_EQ(api.moveAxes.size(), 2u);
    CHECK_EQ(api.jogStops, 0);

    api.moveResult = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    jc.tick();  // retried without new wheel input
    CHECK_EQ(api.moveAxes.size(), 3u);
    CHECK_NEAR(api.moveAxes[2].value, 3.0, 1e-9);
    CHECK_EQ(api.jogStops, 0);
}

static void test_move_blocked_while_program_runs() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);

    // TNG reports our own MoveAxis as running: the wheel move continues.
    state.machine.running = true;
    state.machine.idle = false;
    jc.tick();
    CHECK_EQ(api.jogStops, 0);

    // A paused program aborts the move and blocks new wheel input.
    state.machine.paused = true;
    jc.tick();
    CHECK_EQ(api.jogStops, 1);
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);

    // Something already running when the wheel turns: input is dropped.
    state.machine.paused = false;
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);
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

static void test_move_blocked_by_estop() {
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
static void test_move_checks_live_estop() {
    MockTngApi api;
    SharedState state;
    JoggingConfig cfg;
    api.motorX = 0.0;
    state.pendant.axisCode = xhc::kAxisX;
    state.stepSize = 1.0;

    JogController jc(api, state, cfg);
    api.estop = true;  // shared state still says no e-stop
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 0u);

    api.estop = false;
    state.pendant.jogCounts.store(5);
    jc.tick();
    CHECK_EQ(api.moveAxes.size(), 1u);

    api.estop = true;  // e-stop while the move is in flight
    jc.tick();
    CHECK_EQ(api.jogStops, 1);
}

static void test_move_disabled_in_attach_mode() {
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
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    addButton(cfg, "reset", "estop");
    ButtonHandler bh(api, state, cfg);
    CHECK(bh.onPress("reset"));
    CHECK_EQ(api.estopToggles, 1);
}

static void test_button_stop_start_pause() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
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
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    addButton(cfg, "start_pause", "toggle_start_pause");
    ButtonHandler bh(api, state, cfg);

    // Paused -> start.
    api.paused = true;
    CHECK(bh.onPress("start_pause"));
    CHECK_EQ(api.starts, 1);
    CHECK_EQ(api.pauses.size(), 0u);

    // Idle and not running -> start.
    api.paused = false;
    api.idle = true;
    api.running = false;
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
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
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
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    cfg.buttons.emplace_back("macro_1", makeButtonAction("feed_override", "", 20.0));
    api.params["_ovrd_speedfeed"] = 1.0;
    ButtonHandler bh(api, state, cfg);

    CHECK(bh.onPress("macro_1"));
    CHECK_NEAR(api.params["_ovrd_speedfeed"], 1.2, 1e-9);
}

static void test_button_gcode() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    cfg.buttons.emplace_back("macro_2", makeButtonAction("gcode", "G0 X10"));
    ButtonHandler bh(api, state, cfg);

    CHECK(bh.onPress("macro_2"));
    CHECK_EQ(api.startCodes.size(), 1u);
    CHECK_EQ(api.startCodes[0], std::string("G0 X10"));
}

static void test_button_step_size_cycles() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
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
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
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

    // 2 clicks x 1 mm -> MoveAxis to X = 2.
    CHECK_EQ(api.moveAxes.size(), 1u);
    CHECK_NEAR(api.moveAxes[0].value, 2.0, 1e-9);
}

static void test_button_override_without_reading() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    cfg.buttons.emplace_back("macro_1", makeButtonAction("feed_override", "", 20.0));
    // `_ovrd_speedfeed` is not readable (e.g. TNG not initialized yet).
    ButtonHandler bh(api, state, cfg);

    CHECK(!bh.onPress("macro_1"));
    CHECK_EQ(api.setParams.size(), 0u);
}

static void test_button_noop_and_unbound() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    addButton(cfg, "macro_3", "noop");
    ButtonHandler bh(api, state, cfg);

    CHECK(bh.onPress("macro_3"));
    CHECK(!bh.onPress("does_not_exist"));
}

static void test_button_unknown_action() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
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
    api.workX = 1.0;
    api.workY = 2.0;
    api.workZ = 3.0;
    api.motorX = 4.0;
    api.motorY = 5.0;
    api.motorZ = 6.0;
    api.workA = 7.0;
    api.motorA = 8.0;
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
    CHECK_NEAR(state.machine.workA, 7.0, 1e-9);
    CHECK_NEAR(state.machine.motorA, 8.0, 1e-9);
    CHECK_NEAR(state.machine.feed, 10.0, 1e-9);
    CHECK_NEAR(state.machine.spindle, 200.0, 1e-9);
    CHECK_EQ(state.machine.jogPot, 0x12u);
    CHECK(state.machine.controllerReady);
    CHECK(!state.machine.idle);
    CHECK(state.machine.running);
    CHECK(state.machine.paused);
    CHECK(state.machine.initialized);
    CHECK(!state.machine.estop);

    // Idle machine: IsRunning stays true (TNG alive) but no program runs.
    api.idle = true;
    api.running = false;
    api.paused = false;
    CHECK(sr.read());
    CHECK(!state.machine.running);
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
    return static_cast<uint16_t>(payloadByte(f, off) | (payloadByte(f, off + 1) << 8));
}

static void test_display_skip_when_axis_off() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    state.pendant.axisCode = xhc::kAxisOff;
    DisplayUpdater du(api, state, cfg);
    DisplayUpdater::Frame f;
    CHECK(!du.build(f));
}

static void test_display_always_sends_when_axis_off() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    cfg.polling.displayAlways = true;
    state.pendant.axisCode = xhc::kAxisOff;
    DisplayUpdater du(api, state, cfg);
    DisplayUpdater::Frame f;
    CHECK(du.build(f));
}

static void test_display_forced_frame_when_axis_off() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    state.pendant.axisCode = xhc::kAxisOff;
    DisplayUpdater du(api, state, cfg);
    DisplayUpdater::Frame f;
    CHECK(du.build(f, /*force=*/true));
}

static void test_display_a_axis() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    state.pendant.axisCode = xhc::kAxisA;
    state.machine.workX = 1.0;
    state.machine.workA = 45.5;
    state.machine.motorA = 90.25;
    DisplayUpdater du(api, state, cfg);
    DisplayUpdater::Frame f;
    CHECK(du.build(f));
    // Line 1 shows work A, not X.
    CHECK_EQ(readLE16At(f, 3), 45);
    CHECK_EQ(readLE16At(f, 5), 5000);

    // Without a work position the machine position is shown instead.
    state.machine.workA = std::numeric_limits<double>::quiet_NaN();
    CHECK(du.build(f));
    CHECK_EQ(readLE16At(f, 3), 90);
    CHECK_EQ(readLE16At(f, 5), 2500);
}

static void test_display_frame_encoding() {
    MockTngApi api;
    SharedState state;
    Config cfg = makeConfig();
    api.params["_ovrd_speedfeed"] = 1.0;
    api.params["_ovrd_spindle"] = 0.5;
    state.machine.feed = 50.0;
    state.machine.spindle = 200.0;
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
    test_move_single_click();
    test_move_small_click_not_ignored();
    test_move_negative_direction();
    test_move_a_axis();
    test_move_stops_at_target_no_oscillation();
    test_move_coalesces_rapid_ticks();
    test_move_settles_to_new_anchor();
    test_move_input_not_dropped_at_settle();
    test_move_no_drive_back_after_external_move();
    test_move_reset_after_estop();
    test_move_stops_on_axis_change();
    test_move_needs_valid_position();
    test_move_stops_on_failure();
    test_move_rejected_reissue_retried();
    test_move_sub_resolution_steps_accumulate();
    test_move_blocked_while_program_runs();
    test_override_allowed_while_program_runs();
    test_move_blocked_by_estop();
    test_move_checks_live_estop();
    test_move_disabled_in_attach_mode();
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
    test_display_forced_frame_when_axis_off();
    test_display_a_axis();
    test_display_always_sends_when_axis_off();
    test_display_frame_encoding();

    return tfw::summary("test_planetcnc");
}
