#include "planetcnc/TngApi.h"

#include "utils/Logger.h"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
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
    std::wstring w(path.begin(), path.end());
    return reinterpret_cast<void*>(LoadLibraryW(w.c_str()));
}

void* getSymbol(void* h, const char* name) {
    return reinterpret_cast<void*>(
        GetProcAddress(reinterpret_cast<HMODULE>(h), name));
}

void freeLibrary(void* h) { FreeLibrary(reinterpret_cast<HMODULE>(h)); }

const char* libDefaultName() { return "PlanetCNCLib64.dll"; }
#else
void* loadLibrary(const std::string& path) {
    if (path.empty()) {
        return dlopen("libPlanetCNCLib64.so", RTLD_NOW);
    }
    return dlopen(path.c_str(), RTLD_NOW);
}

void* getSymbol(void* h, const char* name) { return dlsym(h, name); }

void freeLibrary(void* h) { dlclose(h); }

const char* libDefaultName() { return "libPlanetCNCLib64.so"; }
#endif

// Process-wide TNG initialization state, updated from the CDECL callback.
std::atomic<int> g_tngInitState{0};

void initCallback(int value) { g_tngInitState.store(value); }
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
        error = std::string("failed to load TNG library (")
              + (libPath.empty() ? libDefaultName() : libPath) + ")";
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

    resolve("Jog", fnJog_, true, error);
    resolve("Jog9", fnJog9_, false, error);
    resolve("JogStop", fnJogStop_, true, error);
    resolve("MoveAxis", fnMoveAxis_, false, error);

    resolve("SetInitialiseCB", fnSetInitialiseCB_, false, error);
    resolve("SetRefreshCB", fnSetRefreshCB_, false, error);
    resolve("SetIdleCB", fnSetIdleCB_, false, error);
    resolve("SetLineNumCB", fnSetLineNumCB_, false, error);

    if (!error.empty()) {
        logWarn("TNG API: some symbols failed to resolve: %s", error.c_str());
        error.clear();
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
    fnRun_ = nullptr; fnRunProfile_ = nullptr; fnExit_ = nullptr;
    fnExitForce_ = nullptr; fnGetVer_ = nullptr;
    fnIsRunning_ = nullptr; fnIsRunningExt_ = nullptr; fnIsInitialized_ = nullptr;
    fnIsControllerReady_ = nullptr; fnIsIdle_ = nullptr; fnIsEStop_ = nullptr;
    fnIsStop_ = nullptr; fnIsPause_ = nullptr;
    fnEStop_ = nullptr; fnEStopToggle_ = nullptr; fnStop_ = nullptr;
    fnPause_ = nullptr; fnPauseToggle_ = nullptr; fnStart_ = nullptr;
    fnGetCmdId_ = nullptr; fnCmdExec_ = nullptr; fnCmdExecStr_ = nullptr;
    fnCmdExecVal_ = nullptr; fnSetParam_ = nullptr; fnGetParam_ = nullptr;
    fnStartCode_ = nullptr; fnOpenCode_ = nullptr;
    fnInfoSpeed_ = nullptr; fnInfoSpindle_ = nullptr; fnInfoJogPot_ = nullptr;
    fnInfoWorkPosition3_ = nullptr; fnInfoMotorPosition3_ = nullptr;
    fnJog_ = nullptr; fnJog9_ = nullptr; fnJogStop_ = nullptr; fnMoveAxis_ = nullptr;
    fnSetInitialiseCB_ = nullptr; fnSetRefreshCB_ = nullptr;
    fnSetIdleCB_ = nullptr; fnSetLineNumCB_ = nullptr;
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
bool TngApi::run(bool hideUI) {
    if (!fnRun_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnRun_(hideUI) == 0;
}

bool TngApi::runProfile(bool hideUI, const std::string& profile) {
    if (!fnRunProfile_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnRunProfile_(hideUI, profile.c_str()) == 0;
}

void TngApi::exitTng() {
    if (!fnExit_) return;
    std::lock_guard<std::mutex> lk(mtx_);
    fnExit_();
}

void TngApi::exitTngForce() {
    if (!fnExitForce_) return;
    std::lock_guard<std::mutex> lk(mtx_);
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
int TngApi::getCmdCount() {
    return 0; // GetCmdCount intentionally not bound in v1
}
int TngApi::getCmdId(const std::string& name) {
    if (!fnGetCmdId_) return -1;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnGetCmdId_(name.c_str());
}
bool TngApi::isCmdEnabled(int id) {
    (void)id;
    return true; // IsCmdEnabled not bound in v1
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
double TngApi::getParam(const std::string& name) {
    if (!fnGetParam_) return 0.0;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnGetParam_(name.c_str());
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

// --- Jog / move ------------------------------------------------------------
bool TngApi::jog(bool step, double x, double y, double z) {
    if (!fnJog_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return fnJog_(step, x, y, z);
}
bool TngApi::jog9(bool step, double x, double y, double z,
                  double a, double b, double c, double u, double v, double w) {
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
