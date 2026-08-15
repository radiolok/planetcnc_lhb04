#pragma once

#include <string>

namespace mpgd {

// Abstract interface to the PlanetCNC TNG API. `TngApi` is the production
// implementation backed by LoadLibrary/GetProcAddress; unit tests substitute a
// mock so the jog, button, state-read and display logic can be verified without
// the vendor SDK. Only the surface actually used by the logic layer is
// declared here.
class ITngApi {
public:
    virtual ~ITngApi() = default;

    // --- Run status --------------------------------------------------------
    virtual bool isRunning() = 0;
    virtual bool isInitialized() = 0;
    virtual bool isControllerReady() = 0;
    virtual bool isIdle() = 0;
    virtual bool isEStop() = 0;
    virtual bool isPause() = 0;

    // --- Machine commands --------------------------------------------------
    virtual bool estopToggle() = 0;
    virtual bool stop() = 0;
    virtual bool start() = 0;
    virtual bool pause(bool on) = 0;
    virtual bool pauseToggle() = 0;

    // --- Generic commands --------------------------------------------------
    virtual int getCmdId(const std::string& name) = 0;
    virtual bool cmdExec(int id) = 0;

    // --- Parameters --------------------------------------------------------
    virtual bool setParam(const std::string& name, double value) = 0;
    virtual double getParam(const std::string& name) = 0;

    // --- G-code helpers ----------------------------------------------------
    virtual bool startCode(const std::string& gcode) = 0;

    // --- Info --------------------------------------------------------------
    virtual double infoSpeed() = 0;
    virtual double infoSpindle() = 0;
    virtual unsigned infoJogPot() = 0;
    virtual bool infoWorkPosition3(double& x, double& y, double& z) = 0;
    virtual bool infoMotorPosition3(double& x, double& y, double& z) = 0;

    // --- Jog / move --------------------------------------------------------
    virtual bool jog(bool step, double x, double y, double z) = 0;
    virtual bool jog9(bool step, double x, double y, double z,
                      double a, double b, double c,
                      double u, double v, double w) = 0;
    virtual bool jogStop() = 0;
};

} // namespace mpgd
