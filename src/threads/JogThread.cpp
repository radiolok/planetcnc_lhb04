#include "threads/JogThread.h"

#include "logic/JogController.h"
#include "utils/Logger.h"

#include <chrono>
#include <thread>

namespace mpgd {

JogThread::JogThread(SharedState& state, JogController& controller, int periodMs)
    : state_(state), controller_(controller), periodMs_(periodMs) {}

void JogThread::run() {
    logInfo("jog thread started (period=%dms)", periodMs_);
    while (!state_.shutdown.load()) {
        controller_.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(periodMs_));
    }
    controller_.stopNow();
    logInfo("jog thread stopped");
}

} // namespace mpgd
