#include "threads/ButtonThread.h"

#include "logic/ButtonHandler.h"
#include "logic/ButtonQueue.h"
#include "utils/Logger.h"

#include <chrono>

namespace mpgd {

ButtonThread::ButtonThread(SharedState& state, ButtonQueue& queue, ButtonHandler& handler)
    : state_(state), queue_(queue), handler_(handler) {}

void ButtonThread::run() {
    logInfo("button thread started");
    while (!state_.shutdown.load()) {
        // Short timeout so shutdown is noticed promptly.
        if (auto name = queue_.popFor(std::chrono::milliseconds(100))) {
            handler_.onPress(*name);
        }
    }
    logInfo("button thread stopped");
}

} // namespace mpgd
