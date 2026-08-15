#pragma once

#include "TNG_API.h"

#include <atomic>
#include <mutex>
#include <string>

namespace mpgd {

// Runtime binding to the PlanetCNC TNG API. The SDK library
// (PlanetCNCLib64.dll on Windows, libPlanetCNCLib64.so on Linux) is loaded
// dynamically and all entry points are resolved with GetProcAddress/dlsym, so
// the project does not depend on a vendor header or import library.
//
// Every call is serialized through a single mutex because the SDK is not
// documented to be thread-safe while USB polling, jog processing and display
// updates run concurrently.
class TngApi {
public:
    TngApi() = default;
    ~TngApi();

    TngApi(const TngApi&) = delete;
    TngApi& operator=(const TngApi&) = delete;

    // Loads the library and resolves symbols. `libPath` may be empty to use
    // the platform default name. Returns true on success and sets `error` on
    // failure. GetVer() must return non-zero for the library to be considered
    // usable.
    bool load(const std::string& libPath, std::string& error);

    void unload();
    bool isLoaded() const { return handle_ != nullptr; }

    // --- Run & exit --------------------------------------------------------
    bool run(bool hideUI);
    bool runProfile(bool hideUI, const std::string& profile);
    void exitTng();
    void exitTngForce();
    int getVer();

    // --- Run status --------------------------------------------------------
    bool isRunning();
    bool isRunningExt();
    bool isInitialized();
    bool isControllerReady();
    bool isIdle();
    bool isEStop();
    bool isStop();
    bool isPause();

    // --- Machine commands --------------------------------------------------
    bool estop(bool on);
    bool estopToggle();
    bool stop();
    bool pause(bool on);
    bool pauseToggle();
    bool start();

    // --- Generic commands --------------------------------------------------
    int getCmdCount();
    int getCmdId(const std::string& name);
    bool isCmdEnabled(int id);
    bool cmdExec(int id);
    bool cmdExecStr(int id, const std::string& str);
    bool cmdExecVal(int id, double val);

    // --- Parameters --------------------------------------------------------
    bool setParam(const std::string& name, double value);
    double getParam(const std::string& name);

    // --- G-code helpers ----------------------------------------------------
    bool startCode(const std::string& gcode);
    bool openCode(const std::string& gcode);

    // --- Info --------------------------------------------------------------
    double infoSpeed();
    double infoSpindle();
    unsigned infoJogPot();
    bool infoWorkPosition3(double& x, double& y, double& z);
    bool infoMotorPosition3(double& x, double& y, double& z);

    // --- Jog / move --------------------------------------------------------
    bool jog(bool step, double x, double y, double z);
    bool jog9(bool step, double x, double y, double z,
              double a, double b, double c, double u, double v, double w);
    bool jogStop();
    bool moveAxis(double speed, int axis, double value);

    // --- Callbacks ---------------------------------------------------------
    // Installed with process-lifetime static callbacks (see .cpp). The init
    // callback updates a process-wide atomic readable via initState().
    void installCallbacks();
    int initState() const;

private:
    template <typename Fn>
    Fn resolve(const char* name, Fn& slot, bool required, std::string& error);

    void* handle_ = nullptr;
    std::mutex mtx_;

    // Import table (see include/TNG_API.h for typedefs).
    tng::Fn_Run             fnRun_ = nullptr;
    tng::Fn_RunProfile      fnRunProfile_ = nullptr;
    tng::Fn_Exit            fnExit_ = nullptr;
    tng::Fn_Exit            fnExitForce_ = nullptr;
    tng::Fn_GetVer          fnGetVer_ = nullptr;
    tng::Fn_BoolVoid        fnIsRunning_ = nullptr;
    tng::Fn_BoolVoid        fnIsRunningExt_ = nullptr;
    tng::Fn_BoolVoid        fnIsInitialized_ = nullptr;
    tng::Fn_BoolVoid        fnIsControllerReady_ = nullptr;
    tng::Fn_BoolVoid        fnIsIdle_ = nullptr;
    tng::Fn_BoolVoid        fnIsEStop_ = nullptr;
    tng::Fn_BoolVoid        fnIsStop_ = nullptr;
    tng::Fn_BoolVoid        fnIsPause_ = nullptr;
    tng::Fn_BoolBool        fnEStop_ = nullptr;
    tng::Fn_BoolVoid        fnEStopToggle_ = nullptr;
    tng::Fn_BoolVoid        fnStop_ = nullptr;
    tng::Fn_BoolBool        fnPause_ = nullptr;
    tng::Fn_BoolVoid        fnPauseToggle_ = nullptr;
    tng::Fn_BoolVoid        fnStart_ = nullptr;
    tng::Fn_GetCmdId        fnGetCmdId_ = nullptr;
    tng::Fn_CmdExec         fnCmdExec_ = nullptr;
    tng::Fn_CmdExecStr      fnCmdExecStr_ = nullptr;
    tng::Fn_CmdExecVal      fnCmdExecVal_ = nullptr;
    tng::Fn_SetParam        fnSetParam_ = nullptr;
    tng::Fn_GetParam        fnGetParam_ = nullptr;
    tng::Fn_StartCode       fnStartCode_ = nullptr;
    tng::Fn_OpenCode        fnOpenCode_ = nullptr;
    tng::Fn_InfoSpeed       fnInfoSpeed_ = nullptr;
    tng::Fn_InfoSpeed       fnInfoSpindle_ = nullptr;
    tng::Fn_InfoJogPot      fnInfoJogPot_ = nullptr;
    tng::Fn_InfoPos3        fnInfoWorkPosition3_ = nullptr;
    tng::Fn_InfoPos3        fnInfoMotorPosition3_ = nullptr;
    tng::Fn_Jog             fnJog_ = nullptr;
    tng::Fn_Jog9            fnJog9_ = nullptr;
    tng::Fn_JogStop         fnJogStop_ = nullptr;
    tng::Fn_MoveAxis        fnMoveAxis_ = nullptr;
    tng::Fn_SetInitialiseCB fnSetInitialiseCB_ = nullptr;
    tng::Fn_SetRefreshCB    fnSetRefreshCB_ = nullptr;
    tng::Fn_SetIdleCB       fnSetIdleCB_ = nullptr;
    tng::Fn_SetLineNumCB    fnSetLineNumCB_ = nullptr;
};

} // namespace mpgd
