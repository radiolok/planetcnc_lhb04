#include "threads/JogThread.h"

#include "logic/JogController.h"
#include "utils/Logger.h"
#include "utils/PeriodicTimer.h"

#include <chrono>

namespace mpgd {

JogThread::JogThread(SharedState& state, JogController& controller, int periodMs)
    : state_(state), controller_(controller), periodMs_(periodMs) {}

void JogThread::run() {
    logInfo("jog thread started (period=%dms)", periodMs_);
    PeriodicTimer timer{std::chrono::milliseconds(periodMs_)};
    while (!state_.shutdown.load()) {
        controller_.tick();
        timer.wait();
    }
    controller_.stopNow();
    logInfo("jog thread stopped");
}

} // namespace mpgd
