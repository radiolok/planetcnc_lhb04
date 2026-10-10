// tng_probe — read-only diagnostic utility for the PlanetCNC TNG C API.
//
// Loads PlanetCNCLib64.dll / libPlanetCNCLib64.so through the same TngApi
// wrapper as the daemon and prints controller state, resolves parameter names
// (GetParam) and command names (GetCmdId). A gated `jog` subcommand issues a
// single Jog()/JogStop() so the Jog semantics can be validated on real
// hardware; it is only available in-process (not --attach) and requires an
// explicit axis/value.
//
// This tool is for Stage 3 (real-controller validation). Read-only commands
// are safe against a running machine; the `jog` command moves axes and must be
// used with the machine free to move.

#include "planetcnc/TngApi.h"
#include "utils/Logger.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string libPath;
    std::string profile;
    bool attach = false;
    bool gui = false;
    bool help = false;
    std::string command = "status";
    std::vector<std::string> args;
    double setStep = 0.0;
    bool hasSetStep = false;
    double moveDelta = 0.0;
    bool hasMoveDelta = false;
    double moveSpeed = 20.0;
};

void printUsage(const char* argv0) {
    std::printf(
        "tng_probe - PlanetCNC TNG API diagnostic (Stage 3 validation)\n"
        "\n"
        "Usage: %s [options] <command> [args]\n"
        "\n"
        "Commands:\n"
        "  status              Dump API version, controller state, positions\n"
        "                      and speeds (default).\n"
        "  params [name...]    GetParam() each name (defaults: documented\n"
        "                      override/jog parameter names).\n"
        "  commands [name...]  GetCmdId() each command name (defaults: button\n"
        "                      command names used by mpgd).\n"
        "  jog                 Issue one Jog() and JogStop() (moves axes).\n"
        "                      Requires --axis and --value (see below).\n"
        "  move                MoveAxis() to position (moves axes). Requires\n"
        "                      --axis and --delta (see below).\n"
        "\n"
        "Options:\n"
        "  --lib <path>        Library path (default: auto-detect)\n"
        "  --profile <name>    Profile for in-process mode\n"
        "  --attach            Attach to an already-running TNG via named pipe\n"
        "  --gui               Run TNG with visible UI in-process (default headless)\n"
        "  --axis <X|Y|Z|A>    Axis for the jog/move command\n"
        "  --value <num>       Distance (mm, step) or speed (mm/min, cont)\n"
        "  --delta <num>       Relative distance for the move command (mm)\n"
        "  --speed <num>       Speed for the move command\n"
        "  --mode <step|cont>  Jog mode for the jog command (default: step)\n"
        "  --seconds <num>     Continuous jog duration (default: 1.0)\n"
        "  --setstep <num>     Set _jog_step before a step jog (mm)\n"
        "  --help              Show this help\n",
        argv0);
}

bool parseInt(const std::string& s, int& out) {
    try { size_t p = 0; out = std::stoi(s, &p); return p == s.size(); }
    catch (...) { return false; }
}

int axisIndex(const std::string& s) {
    if (s == "X" || s == "x") return 0;
    if (s == "Y" || s == "y") return 1;
    if (s == "Z" || s == "z") return 2;
    if (s == "A" || s == "a") return 3;
    return -1;
}

bool parseArgs(int argc, char** argv, Options& opts, std::string& jogMode,
               int& jogAxis, double& jogValue, bool& hasJogValue,
               double& jogSeconds) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--lib") { const char* v = need("--lib"); if (!v) return false; opts.libPath = v; }
        else if (a == "--profile") { const char* v = need("--profile"); if (!v) return false; opts.profile = v; }
        else if (a == "--attach") { opts.attach = true; }
        else if (a == "--gui") { opts.gui = true; }
        else if (a == "--axis") { const char* v = need("--axis"); if (!v) return false; jogAxis = axisIndex(v); if (jogAxis < 0) { std::fprintf(stderr, "error: bad --axis '%s'\n", v); return false; } }
        else if (a == "--value") { const char* v = need("--value"); if (!v) return false; try { jogValue = std::stod(v); hasJogValue = true; } catch (...) { std::fprintf(stderr, "error: bad --value '%s'\n", v); return false; } }
        else if (a == "--mode") { const char* v = need("--mode"); if (!v) return false; jogMode = v; if (jogMode != "step" && jogMode != "cont") { std::fprintf(stderr, "error: bad --mode '%s'\n", v); return false; } }
        else if (a == "--seconds") { const char* v = need("--seconds"); if (!v) return false; try { jogSeconds = std::stod(v); if (jogSeconds <= 0) { std::fprintf(stderr, "error: --seconds must be > 0\n"); return false; } } catch (...) { std::fprintf(stderr, "error: bad --seconds '%s'\n", v); return false; } }
        else if (a == "--setstep") { const char* v = need("--setstep"); if (!v) return false; try { opts.setStep = std::stod(v); opts.hasSetStep = true; } catch (...) { std::fprintf(stderr, "error: bad --setstep '%s'\n", v); return false; } }
        else if (a == "--delta") { const char* v = need("--delta"); if (!v) return false; try { opts.moveDelta = std::stod(v); opts.hasMoveDelta = true; } catch (...) { std::fprintf(stderr, "error: bad --delta '%s'\n", v); return false; } }
        else if (a == "--speed") { const char* v = need("--speed"); if (!v) return false; try { opts.moveSpeed = std::stod(v); } catch (...) { std::fprintf(stderr, "error: bad --speed '%s'\n", v); return false; } }
        else if (a == "--help" || a == "-h") { opts.help = true; }
        else if (!a.empty() && a[0] == '-' && a.size() > 1) { std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str()); return false; }
        else if (opts.command == "status" && opts.args.empty()) { opts.command = a; }
        else { opts.args.push_back(a); }
    }
    return true;
}

void printState(mpgd::TngApi& api) {
    std::printf("== status ==\n");
    std::printf("GetVer          : %d\n", api.getVer());
    std::printf("initState       : %d\n", api.initState());
    std::printf("isInitialized   : %s\n", api.isInitialized() ? "true" : "false");
    std::printf("isRunning       : %s\n", api.isRunning() ? "true" : "false");
    std::printf("isRunningExt    : %s\n", api.isRunningExt() ? "true" : "false");
    std::printf("isControllerReady: %s\n", api.isControllerReady() ? "true" : "false");
    std::printf("isIdle          : %s\n", api.isIdle() ? "true" : "false");
    std::printf("isEStop         : %s\n", api.isEStop() ? "true" : "false");
    std::printf("isStop          : %s\n", api.isStop() ? "true" : "false");
    std::printf("isPause         : %s\n", api.isPause() ? "true" : "false");

    double wx = 0, wy = 0, wz = 0, mx = 0, my = 0, mz = 0;
    if (api.infoWorkPosition3(wx, wy, wz))
        std::printf("work position   : X=%.4f Y=%.4f Z=%.4f\n", wx, wy, wz);
    else
        std::printf("work position   : unavailable\n");
    if (api.infoMotorPosition3(mx, my, mz))
        std::printf("motor position  : X=%.4f Y=%.4f Z=%.4f\n", mx, my, mz);
    else
        std::printf("motor position  : unavailable\n");

    std::printf("InfoSpeed       : %.4f\n", api.infoSpeed());
    std::printf("InfoSpindle     : %.4f\n", api.infoSpindle());
    std::printf("InfoJogPot      : %u\n", api.infoJogPot());
}

// Parameter names from the PlanetCNC TNG G-code reference ("Settings - Jogging",
// "Settings - Program Options"). The legacy names are the previous mpgd guess.
const std::vector<std::pair<const char*, const char*>>& probeParams() {
    static const std::vector<std::pair<const char*, const char*>> p = {
        {"_ovrd_speedfeed",   "feed override (documented)"},
        {"_ovrd_spindle",     "spindle override (documented)"},
        {"_ovrd_speedtraverse","traverse override (documented)"},
        {"_jog_speed",        "jog speed (documented)"},
        {"_jog_speeddef",     "jog speed default (documented)"},
        {"_jog_step",         "jog step (documented)"},
        {"_jog_stepdef",      "jog step default (documented)"},
        {"_jog_round",        "jog rounding (documented)"},
        {"_speed_feed",       "feed speed (documented)"},
        {"_speed_spindle",    "spindle speed (documented)"},
        {"_speed_traverse",   "traverse speed (documented)"},
        {"SpeedFeedOverride", "feed override (legacy mpgd guess)"},
        {"SpeedSpindleOverride","spindle override (legacy mpgd guess)"},
    };
    return p;
}

void printParams(mpgd::TngApi& api, const std::vector<std::string>& names) {
    std::printf("== params (GetParam) ==\n");
    std::vector<std::pair<const char*, const char*>> list;
    if (names.empty()) {
        list = probeParams();
    } else {
        for (const auto& n : names) list.emplace_back(n.c_str(), "");
    }
    for (const auto& [name, note] : list) {
        const std::optional<double> v = api.getParam(name);
        if (v) {
            std::printf("  %-22s = %-12.4f   %s\n", name, *v, note);
        } else {
            std::printf("  %-22s = %-12s   %s\n", name, "n/a", note);
        }
    }
}

// Command names taken from the PlanetCNC "Handwheel" example profile; these
// are the ones mpgd's ButtonHandler relies on.
const std::vector<const char*>& probeCommands() {
    static const std::vector<const char*> c = {
        "Machine.Emergency_Stop",
        "Machine.Start",
        "Machine.Pause",
        "Machine.Stop",
        "Machine.Home",
        "Machine.Spindle",
        "Machine.Flood",
        "Machine.Mist",
        "Machine.Work_Position.Offset.To_Zero",
        "Machine.Work_Position.Axis_To_Zero.XY",
        "Machine.Work_Position.Axis_To_Zero.Z",
        "Machine.Work_Position.Measure_Height",
        "Machine.Move.Axis_To_Zero.XY",
        "Machine.Tool_Offset.Measure_Length",
    };
    return c;
}

void printCommands(mpgd::TngApi& api, const std::vector<std::string>& names) {
    std::printf("== commands (GetCmdId) ==\n");
    std::vector<const char*> list;
    if (names.empty()) {
        list = probeCommands();
    } else {
        for (const auto& n : names) list.emplace_back(n.c_str());
    }
    for (const auto* name : list) {
        int id = api.getCmdId(name);
        std::printf("  %-44s id=%d %s\n", name, id, id < 0 ? "(NOT FOUND)" : "");
    }
}

const char* axisName(int axis) {
    switch (axis) {
        case 0: return "X";
        case 1: return "Y";
        case 2: return "Z";
        case 3: return "A";
        default: return "?";
    }
}

struct AxisPos {
    double work[3] = {0, 0, 0};
    double motor[3] = {0, 0, 0};
    bool haveWork = false;
    bool haveMotor = false;
};

AxisPos readPositions(mpgd::TngApi& api) {
    AxisPos p;
    p.haveWork = api.infoWorkPosition3(p.work[0], p.work[1], p.work[2]);
    p.haveMotor = api.infoMotorPosition3(p.motor[0], p.motor[1], p.motor[2]);
    return p;
}

bool waitIdle(mpgd::TngApi& api, int timeoutMs) {
    int waited = 0;
    while (!api.isIdle() && waited < timeoutMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waited += 20;
    }
    return api.isIdle();
}

void reportPosition(const char* tag, const AxisPos& p, int axis) {
    std::printf("  %-8s work  X=% .4f Y=% .4f Z=% .4f\n",
                tag, p.work[0], p.work[1], p.work[2]);
    std::printf("  %-8s motor X=% .4f Y=% .4f Z=% .4f\n",
                tag, p.motor[0], p.motor[1], p.motor[2]);
    (void)axis;
}

bool runJog(mpgd::TngApi& api, const std::string& mode, int axis, double value,
            double seconds, double setStep, bool hasSetStep) {
    if (axis < 0) {
        std::fprintf(stderr, "error: jog requires --axis X|Y|Z|A\n");
        return false;
    }
    const bool step = (mode == "step");
    std::printf("state: controllerReady=%s idle=%s estop=%s stop=%s\n",
                api.isControllerReady() ? "true" : "false",
                api.isIdle() ? "true" : "false",
                api.isEStop() ? "true" : "false",
                api.isStop() ? "true" : "false");
    if (!api.isControllerReady()) {
        std::printf("WARNING: IsControllerReady()=false; jog may not move\n");
    }
    if (api.isEStop()) {
        std::fprintf(stderr, "error: E-Stop is active; aborting\n");
        return false;
    }

    // Enable axis motors (M10 P1). PlanetCNC jog does not move the axes while
    // the motor enable signal is off; the API still returns ok.
    bool motors = api.startCode("M10 P1");
    std::printf("MotorsEnable (M10 P1) -> %s\n", motors ? "ok" : "FAILED");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (hasSetStep) {
        bool s = api.setParam("_jog_step", setStep);
        std::printf("SetParam(_jog_step=%.4f) -> %s\n", setStep, s ? "ok" : "FAILED");
    }

    double v[6] = {0, 0, 0, 0, 0, 0};
    v[axis] = value;

    AxisPos before = readPositions(api);
    std::printf("== jog %s, axis=%s, value=%.4f ==\n",
                step ? "STEP" : "CONT", axisName(axis), value);
    reportPosition("before", before, axis);

    const auto t0 = std::chrono::steady_clock::now();

    bool ok;
    if (axis < 3) ok = api.jog(step, v[0], v[1], v[2]);
    else ok = api.jog9(step, v[0], v[1], v[2], v[3], v[4], v[5], 0, 0, 0);
    std::printf("Jog(step=%s) -> %s\n", step ? "true" : "false",
                ok ? "ok" : "FAILED");
    if (!ok) return false;

    if (step) {
        // Poll position over time to capture the actual move trajectory.
        const int samples = 40;  // 40 * 200ms = 8s
        for (int i = 1; i <= samples; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            AxisPos p = readPositions(api);
            std::printf("  t=%5.1fs  motor[%s]=% .4f  idle=%s\n",
                        i * 0.2, axisName(axis), p.motor[axis],
                        api.isIdle() ? "true" : "false");
        }
    } else {
        // Poll position during the move to capture the cruise speed slope.
        const int sleepMs = static_cast<int>(seconds * 1000.0);
        const int polls = sleepMs / 200;
        for (int i = 1; i <= polls; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            AxisPos p = readPositions(api);
            std::printf("  t=%5.1fs  motor[%s]=% .4f\n",
                        i * 0.2, axisName(axis), p.motor[axis]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs % 200));
        bool stopped = api.jogStop();
        std::printf("JogStop() -> %s\n", stopped ? "ok" : "FAILED");
        bool idled = waitIdle(api, 30000);
        std::printf("waitIdle(after stop) -> %s\n", idled ? "true" : "false");
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                          t1 - t0).count() / 1000.0;

    AxisPos after = readPositions(api);
    reportPosition("after", after, axis);

    if (before.haveMotor && after.haveMotor) {
        const double delta = after.motor[axis] - before.motor[axis];
        if (step) {
            std::printf("STEP result: requested=%.4f mm, motor delta=%+.4f mm\n",
                        value, delta);
        } else {
            std::printf("CONT result: requested=%.4f mm/min, dt=%.3f s, "
                        "motor delta=%+.4f mm, measured speed=%.2f mm/min\n",
                        value, dt, delta, dt > 0 ? (delta / dt) * 60.0 : 0.0);
        }
    } else {
        std::printf("(motor position unavailable; cannot compute delta)\n");
    }

    std::printf("isIdle after stop: %s\n", api.isIdle() ? "true" : "false");
    return true;
}

bool runMove(mpgd::TngApi& api, int axis, double delta, double speed) {
    if (axis < 0) {
        std::fprintf(stderr, "error: move requires --axis X|Y|Z|A\n");
        return false;
    }
    if (!api.isControllerReady()) {
        std::printf("WARNING: IsControllerReady()=false; move may not work\n");
    }
    if (api.isEStop()) {
        std::fprintf(stderr, "error: E-Stop is active; aborting\n");
        return false;
    }

    bool motors = api.startCode("M10 P1");
    std::printf("MotorsEnable (M10 P1) -> %s\n", motors ? "ok" : "FAILED");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    AxisPos before = readPositions(api);
    if (!before.haveMotor) {
        std::fprintf(stderr, "error: motor position unavailable\n");
        return false;
    }
    const double p0 = before.motor[axis];
    const double target = p0 + delta;
    std::printf("== MoveAxis(axis=%s, speed=%.4f, target=%.4f) current=%.4f delta=%.4f ==\n",
                axisName(axis), speed, target, p0, delta);

    bool ok = api.moveAxis(speed, axis, target);
    std::printf("MoveAxis -> %s\n", ok ? "ok" : "FAILED");
    if (!ok) return false;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= 100; ++i) {  // up to 20 s
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        AxisPos p = readPositions(api);
        bool idle = api.isIdle();
        std::printf("  t=%5.1fs  motor[%s]=% .4f  idle=%s\n",
                    i * 0.2, axisName(axis), p.motor[axis],
                    idle ? "true" : "false");
        if (idle) break;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                          t1 - t0).count() / 1000.0;

    AxisPos after = readPositions(api);
    const double actual = after.motor[axis];
    std::printf("MOVE result: target=%.4f, actual=%.4f, delta=%.4f, dt=%.3f s, "
                "avg speed=%.2f mm/s (%.1f mm/min)\n",
                target, actual, actual - p0, dt,
                dt > 0 ? (actual - p0) / dt : 0.0,
                dt > 0 ? (actual - p0) / dt * 60.0 : 0.0);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    std::string jogMode = "step";
    int jogAxis = -1;
    double jogValue = 0.0;
    bool hasJogValue = false;
    double jogSeconds = 1.0;

    if (!parseArgs(argc, argv, opts, jogMode, jogAxis, jogValue, hasJogValue,
                   jogSeconds)) {
        printUsage(argv[0]);
        return 2;
    }
    if (opts.help) {
        printUsage(argv[0]);
        return 0;
    }

    std::string logError;
    mpgd::Logger::init("info", "", logError);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    mpgd::TngApi api;
    std::string err;
    if (!api.load(opts.libPath, err)) {
        mpgd::logError("TNG API unavailable: %s", err.c_str());
        return 1;
    }

    std::thread tngThread;
    std::atomic<bool> tngRunOk{false};

    if (opts.attach) {
        if (!api.isRunningExt()) {
            mpgd::logWarn("attach mode: no external TNG process detected");
        } else {
            mpgd::logInfo("attached to external TNG process");
        }
    } else {
        // Run() blocks: it starts TNG in-process and runs its message loop on
        // the calling thread until Exit(). Run it on a dedicated thread and
        // drive the API from the main thread (validated on real hardware).
        tngThread = std::thread([&] {
            bool ok;
            if (opts.profile.empty()) ok = api.run(!opts.gui);
            else ok = api.runProfile(!opts.gui, opts.profile);
            tngRunOk.store(ok);
        });

        int waited = 0;
        while (!api.isInitialized() && waited < 30000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            waited += 100;
        }
        mpgd::logInfo("TNG %s", api.isInitialized()
                                 ? "initialized" : "not initialized (timeout)");
        if (!api.isInitialized()) {
            mpgd::logError("TNG did not initialize; aborting");
            api.exitTngForce();
            if (tngThread.joinable()) tngThread.join();
            return 1;
        }
    }

    int rc = 0;
    mpgd::logInfo("running command: %s", opts.command.c_str());
    if (opts.command == "status") {
        printState(api);
    } else if (opts.command == "params") {
        printParams(api, opts.args);
    } else if (opts.command == "commands") {
        printCommands(api, opts.args);
    } else if (opts.command == "jog") {
        if (opts.attach) {
            std::fprintf(stderr, "error: jog is not available in attach (pipe) mode\n");
            rc = 1;
        } else if (!hasJogValue) {
            std::fprintf(stderr, "error: jog requires --value <num>\n");
            rc = 2;
        } else if (!runJog(api, jogMode, jogAxis, jogValue, jogSeconds,
                           opts.setStep, opts.hasSetStep)) {
            rc = 1;
        }
    } else if (opts.command == "move") {
        if (opts.attach) {
            std::fprintf(stderr, "error: move is not available in attach (pipe) mode\n");
            rc = 1;
        } else if (!opts.hasMoveDelta) {
            std::fprintf(stderr, "error: move requires --delta <num>\n");
            rc = 2;
        } else if (!runMove(api, jogAxis, opts.moveDelta, opts.moveSpeed)) {
            rc = 1;
        }
    } else {
        std::fprintf(stderr, "error: unknown command '%s'\n", opts.command.c_str());
        printUsage(argv[0]);
        rc = 2;
    }
    mpgd::logInfo("command done (rc=%d)", rc);

    if (!opts.attach) {
        mpgd::logInfo("exiting TNG...");
        api.exitTngForce();
        // Run() may not return promptly after Exit(); give it a moment, then
        // detach (process teardown cleans up the TNG thread).
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (tngThread.joinable()) tngThread.detach();
        mpgd::logInfo("TNG thread detached");
    }
    return rc;
}
