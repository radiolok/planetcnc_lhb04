#include "planetcnc/TngApi.h"

#include "utils/Logger.h"
#include "utils/Utf8.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mpgd {

namespace {

// --- Platform abstraction -------------------------------------------------
// The handle is stored as void* (see TngApi.h) and cast to the platform type
// here.
#if defined(_WIN32)
void* loadLibrary(const std::string& path) {
    if (path.empty()) {
        return reinterpret_cast<void*>(LoadLibraryW(L"PlanetCNCLib64.dll"));
    }
    // The configured path is UTF-8 (YAML); widening byte by byte would
    // corrupt any non-ASCII directory name.
    const std::wstring w = wideFromUtf8(path);
    // LOAD_WITH_ALTERED_SEARCH_PATH makes the loader resolve this DLL's
    // dependencies relative to its own directory. PlanetCNCLib64.dll depends
    // on PlanetCNCCore64.dll which lives next to it, so a full path works
    // without adding the PlanetCNC install dir to PATH.
    return reinterpret_cast<void*>(
        LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

void* getSymbol(void* h, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
}

void freeLibrary(void* h) {
    FreeLibrary(reinterpret_cast<HMODULE>(h));
}

const char* libDefaultName() {
    return "PlanetCNCLib64.dll";
}
#else
void* loadLibrary(const std::string& path) {
    if (path.empty()) {
        return dlopen("libPlanetCNCLib64.so", RTLD_NOW);
    }
    return dlopen(path.c_str(), RTLD_NOW);
}

void* getSymbol(void* h, const char* name) {
    return dlsym(h, name);
}

void freeLibrary(void* h) {
    dlclose(h);
}

const char* libDefaultName() {
    return "libPlanetCNCLib64.so";
}
#endif

// Process-wide TNG initialization state, updated from the CDECL callback.
std::atomic<int> g_tngInitState{0};

void initCallback(int value) {
    g_tngInitState.store(value);
}
void refreshCallback() {}
void idleCallback() {}
void lineNumCallback(int) {}

} // namespace

TngApi::~TngApi() {
    unload();
}

template <typename Fn>
Fn TngApi::resolve(const char* name, Fn& slot, bool required, std::string& error) {
    void* sym = getSymbol(handle_, name);
    if (!sym) {
        if (required) {
            if (!error.empty()) error += "; ";
            error += std::string("missing export: ") + name;
        }
        return nullptr;
    }
    slot = reinterpret_cast<Fn>(sym);
    return slot;
}

bool TngApi::load(const std::string& libPath, std::string& error) {
    unload();

    handle_ = loadLibrary(libPath);
    if (!handle_) {
        error = std::string("failed to load TNG library (") +
                (libPath.empty() ? libDefaultName() : libPath) + ")";
        return false;
    }

    error.clear();

    resolve("Run", fnRun_, true, error);
    resolve("RunProfile", fnRunProfile_, true, error);
    resolve("Exit", fnExit_, true, error);
    resolve("ExitForce", fnExitForce_, true, error);
    resolve("GetVer", fnGetVer_, true, error);

    // dumpbin on the local DLL shows "IsRun" while the manual documents
    // "IsRunning"; resolve both and prefer the documented name.
    resolve("IsRunning", fnIsRunning_, false, error);
    if (!fnIsRunning_) {
        resolve("IsRun", fnIsRunning_, false, error);
    }
    resolve("IsRunningExt", fnIsRunningExt_, false, error);
    resolve("IsInitialized", fnIsInitialized_, false, error);
    resolve("IsControllerReady", fnIsControllerReady_, false, error);
    resolve("IsIdle", fnIsIdle_, false, error);
    resolve("IsEStop", fnIsEStop_, false, error);
    resolve("IsStop", fnIsStop_, false, error);
    resolve("IsPause", fnIsPause_, false, error);

    resolve("EStop", fnEStop_, false, error);
    resolve("EStopToggle", fnEStopToggle_, false, error);
    resolve("Stop", fnStop_, false, error);
    resolve("Pause", fnPause_, false, error);
    resolve("PauseToggle", fnPauseToggle_, false, error);
    resolve("Start", fnStart_, false, error);

    resolve("GetCmdId", fnGetCmdId_, true, error);
    resolve("CmdExec", fnCmdExec_, true, error);
    resolve("CmdExecStr", fnCmdExecStr_, true, error);
    resolve("CmdExecVal", fnCmdExecVal_, true, error);

    resolve("SetParam", fnSetParam_, true, error);
    resolve("GetParam", fnGetParam_, true, error);

    resolve("StartCode", fnStartCode_, false, error);
    resolve("OpenCode", fnOpenCode_, false, error);

    resolve("InfoSpeed", fnInfoSpeed_, true, error);
    resolve("InfoSpindle", fnInfoSpindle_, true, error);
    resolve("InfoJogPot", fnInfoJogPot_, false, error);
    resolve("InfoWorkPosition3", fnInfoWorkPosition3_, true, error);
    resolve("InfoMotorPosition3", fnInfoMotorPosition3_, true, error);
    resolve("InfoMotorPosition", fnInfoMotorPosition_, false, error);
    resolve("InfoWorkPosition", fnInfoWorkPosition_, false, error);

    resolve("Jog", fnJog_, true, error);
    resolve("Jog9", fnJog9_, false, error);
    resolve("JogStop", fnJogStop_, true, error);
    resolve("MoveAxis", fnMoveAxis_, false, error);

    resolve("SetInitialiseCB", fnSetInitialiseCB_, false, error);
    resolve("SetRefreshCB", fnSetRefreshCB_, false, error);
    resolve("SetIdleCB", fnSetIdleCB_, false, error);
    resolve("SetLineNumCB", fnSetLineNumCB_, false, error);

    // `error` only collects exports marked required; mpgd cannot work
    // without them, so refuse the library instead of failing call by call.
    if (!error.empty()) {
        error = "TNG library is missing required exports: " + error;
        unload();
        return false;
    }

    // First call: verify the library is actually usable.
    int ver = getVer();
    if (ver == 0) {
        error = "TNG library GetVer() returned 0; API is not usable";
        unload();
        return false;
    }
    logInfo("TNG API loaded (ver=%d)", ver);

    installCallbacks();
    return true;
}

void TngApi::unload() {
    if (handle_) {
        freeLibrary(handle_);
        handle_ = nullptr;
    }
    // Zero the import table.
    fnRun_ = nullptr;
    fnRunProfile_ = nullptr;
    fnExit_ = nullptr;
    fnExitForce_ = nullptr;
    fnGetVer_ = nullptr;
    fnIsRunning_ = nullptr;
    fnIsRunningExt_ = nullptr;
    fnIsInitialized_ = nullptr;
    fnIsControllerReady_ = nullptr;
    fnIsIdle_ = nullptr;
    fnIsEStop_ = nullptr;
    fnIsStop_ = nullptr;
    fnIsPause_ = nullptr;
    fnEStop_ = nullptr;
    fnEStopToggle_ = nullptr;
    fnStop_ = nullptr;
    fnPause_ = nullptr;
    fnPauseToggle_ = nullptr;
    fnStart_ = nullptr;
    fnGetCmdId_ = nullptr;
    fnCmdExec_ = nullptr;
    fnCmdExecStr_ = nullptr;
    fnCmdExecVal_ = nullptr;
    fnSetParam_ = nullptr;
    fnGetParam_ = nullptr;
    fnStartCode_ = nullptr;
    fnOpenCode_ = nullptr;
    fnInfoSpeed_ = nullptr;
    fnInfoSpindle_ = nullptr;
    fnInfoJogPot_ = nullptr;
    fnInfoWorkPosition3_ = nullptr;
    fnInfoMotorPosition3_ = nullptr;
    fnInfoMotorPosition_ = nullptr;
    fnInfoWorkPosition_ = nullptr;
    fnJog_ = nullptr;
    fnJog9_ = nullptr;
    fnJogStop_ = nullptr;
    fnMoveAxis_ = nullptr;
    fnSetInitialiseCB_ = nullptr;
    fnSetRefreshCB_ = nullptr;
    fnSetIdleCB_ = nullptr;
    fnSetLineNumCB_ = nullptr;
    g_tngInitState.store(0);
}

int TngApi::initState() const {
    return g_tngInitState.load();
}

void TngApi::installCallbacks() {
    if (fnSetInitialiseCB_) fnSetInitialiseCB_(&initCallback);
    if (fnSetRefreshCB_) fnSetRefreshCB_(&refreshCallback);
    if (fnSetIdleCB_) fnSetIdleCB_(&idleCallback);
    if (fnSetLineNumCB_) fnSetLineNumCB_(&lineNumCallback);
}

// --- Run & exit ------------------------------------------------------------
// NOTE: Run()/RunProfile() block for the lifetime of the TNG process (they run
// the TNG message loop on the calling thread and return only after Exit()).
// They must therefore NOT hold mtx_: otherwise every other API call would
// deadlock behind the mutex while Run() is blocked. The lifecycle calls are
// intentionally lock-free; the rest of the API remains serialized via mtx_.
bool TngApi::run(bool hideUI) {
    if (!fnRun_) return false;
    return fnRun_(hideUI) == 0;
}

bool TngApi::runProfile(bool hideUI, const std::string& profile) {
    if (!fnRunProfile_) return false;
    return fnRunProfile_(hideUI, profile.c_str()) == 0;
}

void TngApi::exitTng() {
    if (!fnExit_) return;
    fnExit_();
}

void TngApi::exitTngForce() {
    if (!fnExitForce_) return;
    fnExitForce_();
}

int TngApi::getVer() {
    if (!fnGetVer_) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnGetVer_();
}

// --- Run status ------------------------------------------------------------
bool TngApi::isRunning() {
    if (!fnIsRunning_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnIsRunning_();
}
bool TngApi::isRunningExt() {
    if (!fnIsRunningExt_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnIsRunningExt_();
}
bool TngApi::isInitialized() {
    if (!fnIsInitialized_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnIsInitialized_();
}
bool TngApi::isControllerReady() {
    if (!fnIsControllerReady_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnIsControllerReady_();
}
bool TngApi::isIdle() {
    if (!fnIsIdle_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnIsIdle_();
}
bool TngApi::isEStop() {
    if (!fnIsEStop_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnIsEStop_();
}
bool TngApi::isStop() {
    if (!fnIsStop_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnIsStop_();
}
bool TngApi::isPause() {
    if (!fnIsPause_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnIsPause_();
}

// --- Machine commands ------------------------------------------------------
bool TngApi::estop(bool on) {
    if (!fnEStop_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnEStop_(on);
}
bool TngApi::estopToggle() {
    if (!fnEStopToggle_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnEStopToggle_();
}
bool TngApi::stop() {
    if (!fnStop_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnStop_();
}
bool TngApi::pause(bool on) {
    if (!fnPause_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnPause_(on);
}
bool TngApi::pauseToggle() {
    if (!fnPauseToggle_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnPauseToggle_();
}
bool TngApi::start() {
    if (!fnStart_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnStart_();
}

// --- Generic commands ------------------------------------------------------
int TngApi::getCmdId(const std::string& name) {
    if (!fnGetCmdId_) return -1;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnGetCmdId_(name.c_str());
}
bool TngApi::cmdExec(int id) {
    if (!fnCmdExec_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnCmdExec_(id);
}
bool TngApi::cmdExecStr(int id, const std::string& str) {
    if (!fnCmdExecStr_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnCmdExecStr_(id, str.c_str());
}
bool TngApi::cmdExecVal(int id, double val) {
    if (!fnCmdExecVal_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnCmdExecVal_(id, val);
}

// --- Parameters ------------------------------------------------------------
bool TngApi::setParam(const std::string& name, double value) {
    if (!fnSetParam_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnSetParam_(name.c_str(), value);
}
std::optional<double> TngApi::getParam(const std::string& name) {
    if (!fnGetParam_) return std::nullopt;
    // Before TNG is initialized GetParam does not return real values.
    if (!isInitialized()) return std::nullopt;
    std::lock_guard<std::mutex> lk(mtx_);
    const double v = fnGetParam_(name.c_str());
    // Unknown parameter names read as NaN.
    if (std::isnan(v)) return std::nullopt;
    return v;
}

// --- G-code helpers --------------------------------------------------------
bool TngApi::startCode(const std::string& gcode) {
    if (!fnStartCode_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnStartCode_(gcode.c_str());
}
bool TngApi::openCode(const std::string& gcode) {
    if (!fnOpenCode_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnOpenCode_(gcode.c_str());
}

// --- Info ------------------------------------------------------------------
double TngApi::infoSpeed() {
    if (!fnInfoSpeed_) return 0.0;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnInfoSpeed_();
}
double TngApi::infoSpindle() {
    if (!fnInfoSpindle_) return 0.0;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnInfoSpindle_();
}
unsigned TngApi::infoJogPot() {
    if (!fnInfoJogPot_) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnInfoJogPot_();
}
bool TngApi::infoWorkPosition3(double& x, double& y, double& z) {
    if (!fnInfoWorkPosition3_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnInfoWorkPosition3_(&x, &y, &z);
}
bool TngApi::infoMotorPosition3(double& x, double& y, double& z) {
    if (!fnInfoMotorPosition3_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnInfoMotorPosition3_(&x, &y, &z);
}
double TngApi::infoMotorPosition(int axis) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fnInfoMotorPosition_) return fnInfoMotorPosition_(axis);
    // Fallback: derive X/Y/Z from the 3-axis variant; A/B/C unavailable.
    if (axis < 3 && fnInfoMotorPosition3_) {
        double x = 0.0, y = 0.0, z = 0.0;
        if (fnInfoMotorPosition3_(&x, &y, &z)) {
            return axis == 0 ? x : (axis == 1 ? y : z);
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}
double TngApi::infoWorkPosition(int axis) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fnInfoWorkPosition_) return fnInfoWorkPosition_(axis);
    if (axis < 3 && fnInfoWorkPosition3_) {
        double x = 0.0, y = 0.0, z = 0.0;
        if (fnInfoWorkPosition3_(&x, &y, &z)) {
            return axis == 0 ? x : (axis == 1 ? y : z);
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// --- Jog / move ------------------------------------------------------------
bool TngApi::jog(bool step, double x, double y, double z) {
    if (!fnJog_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnJog_(step, x, y, z);
}
bool TngApi::jog9(bool step, double x, double y, double z, double a, double b, double c, double u,
                  double v, double w) {
    if (!fnJog9_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnJog9_(step, x, y, z, a, b, c, u, v, w);
}
bool TngApi::jogStop() {
    if (!fnJogStop_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnJogStop_();
}
bool TngApi::moveAxis(double speed, int axis, double value) {
    if (!fnMoveAxis_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnMoveAxis_(speed, axis, value);
}

} // namespace mpgd
