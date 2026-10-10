#include "planetcnc/StateReader.h"

#include "planetcnc/ITngApi.h"
#include "utils/Logger.h"

#include <cmath>

namespace mpgd {

bool StateReader::read() {
    if (!api_.isInitialized()) return false;

    double wx = 0, wy = 0, wz = 0;
    double mx = 0, my = 0, mz = 0;
    bool haveWork = api_.infoWorkPosition3(wx, wy, wz);
    bool haveMotor = api_.infoMotorPosition3(mx, my, mz);
    const double wa = api_.infoWorkPosition(3);
    const double ma = api_.infoMotorPosition(3);

    std::lock_guard<std::mutex> lk(state_.mutex);
    if (haveWork) {
        state_.machine.workX = wx;
        state_.machine.workY = wy;
        state_.machine.workZ = wz;
    }
    if (haveMotor) {
        state_.machine.motorX = mx;
        state_.machine.motorY = my;
        state_.machine.motorZ = mz;
    }
    state_.machine.workA = wa;
    if (std::isfinite(ma)) state_.machine.motorA = ma;
    state_.machine.feed = api_.infoSpeed();
    state_.machine.spindle = api_.infoSpindle();
    state_.machine.jogPot = api_.infoJogPot();
    state_.machine.controllerReady = api_.isControllerReady();
    state_.machine.idle = api_.isIdle();
    state_.machine.estop = api_.isEStop();
    // IsRunning (DLL export IsRun) only reports that TNG itself is alive and
    // is always true in-process; IsStop is the "nothing executing" flag
    // (false while moving, during a G4 dwell and while paused).
    const bool stop = api_.isStop();
    state_.machine.running = !stop;
    state_.machine.paused = api_.isPause();
    state_.machine.initialized = api_.isInitialized();

    const int flags = (state_.machine.idle << 0) | (stop << 1) | (state_.machine.paused << 2) |
                      (state_.machine.estop << 3) | (state_.machine.controllerReady << 4);
    if (flags != lastFlags_) {
        logDebug("tng flags: idle=%d stop=%d pause=%d estop=%d ready=%d", state_.machine.idle,
                 stop, state_.machine.paused, state_.machine.estop,
                 state_.machine.controllerReady);
        lastFlags_ = flags;
    }
    return true;
}

} // namespace mpgd
