#include "planetcnc/StateReader.h"

#include "planetcnc/ITngApi.h"

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
    state_.machine.running = api_.isRunning();
    state_.machine.paused = api_.isPause();
    state_.machine.initialized = api_.isInitialized();
    return true;
}

} // namespace mpgd
