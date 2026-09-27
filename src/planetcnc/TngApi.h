#pragma once

#include "planetcnc/ITngApi.h"
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
class TngApi : public ITngApi {
public:
    TngApi() = default;
    ~TngApi() override;

    TngApi(const TngApi&) = delete;
    TngApi& operator=(const TngApi&) = delete;
    TngApi(TngApi&&) = delete;
    TngApi& operator=(TngApi&&) = delete;

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
    bool isRunning() override;
    bool isRunningExt();
    bool isInitialized() override;
    bool isControllerReady() override;
    bool isIdle() override;
    bool isEStop() override;
    bool isStop();
    bool isPause() override;

    // --- Machine commands --------------------------------------------------
    bool estop(bool on);
    bool estopToggle() override;
    bool stop() override;
    bool pause(bool on) override;
    bool pauseToggle() override;
    bool start() override;

    // --- Generic commands --------------------------------------------------
    int getCmdId(const std::string& name) override;
    bool cmdExec(int id) override;
    bool cmdExecStr(int id, const std::string& str);
    bool cmdExecVal(int id, double val);

    // --- Parameters --------------------------------------------------------
    bool setParam(const std::string& name, double value) override;
    std::optional<double> getParam(const std::string& name) override;

    // --- G-code helpers ----------------------------------------------------
    bool startCode(const std::string& gcode) override;
    bool openCode(const std::string& gcode);

    // --- Info --------------------------------------------------------------
    double infoSpeed() override;
    double infoSpindle() override;
    unsigned infoJogPot() override;
    bool infoWorkPosition3(double& x, double& y, double& z) override;
    bool infoMotorPosition3(double& x, double& y, double& z) override;
    double infoMotorPosition(int axis) override;
    double infoWorkPosition(int axis) override;

    // --- Jog / move --------------------------------------------------------
    bool jog(bool step, double x, double y, double z) override;
    bool jog9(bool step, double x, double y, double z,
              double a, double b, double c, double u, double v, double w) override;
    bool jogStop() override;
    bool moveAxis(double speed, int axis, double value) override;

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
    tng::Fn_InfoPosAxis     fnInfoMotorPosition_ = nullptr;
    tng::Fn_InfoPosAxis     fnInfoWorkPosition_ = nullptr;
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
