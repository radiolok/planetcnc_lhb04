#include "threads/DisplayThread.h"

#include "logic/DisplayUpdater.h"
#include "planetcnc/StateReader.h"
#include "usb/HidDevice.h"
#include "utils/Logger.h"
#include "utils/PeriodicTimer.h"

#include <chrono>

namespace mpgd {

DisplayThread::DisplayThread(SharedState& state, usb::HidDevice& device,
                             DisplayUpdater& updater, StateReader& stateReader,
                             int periodMs)
    : state_(state), device_(device), updater_(updater),
      stateReader_(stateReader), periodMs_(periodMs) {}

void DisplayThread::run() {
    logInfo("display thread started (period=%dms)", periodMs_);
    bool wasOpen = false;
    PeriodicTimer timer{std::chrono::milliseconds(periodMs_)};
    while (!state_.shutdown.load()) {
        bool isOpen = device_.isOpen();
        if (isOpen && !wasOpen) {
            // FR-01.3: initial "hello" frame on (re)connect.
            logInfo("display: sending hello frame");
        }
        // Refresh machine state (positions, e-stop, idle) whether or not the
        // pendant is connected: the jog thread relies on it being current.
        stateReader_.read();
        if (isOpen) {
            DisplayUpdater::Frame frame{};
            if (updater_.build(frame)) {
                for (const auto& report : frame) {
                    int r = device_.sendFeatureReport(report.data(), report.size());
                    if (r < 0) {
                        logDebug("display write failed (%d)", r);
                        break;
                    }
                }
            }
        }
        wasOpen = isOpen;
        timer.wait();
    }
    logInfo("display thread stopped");
}

} // namespace mpgd
