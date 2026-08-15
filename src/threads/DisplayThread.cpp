#include "threads/DisplayThread.h"

#include "logic/DisplayUpdater.h"
#include "planetcnc/StateReader.h"
#include "usb/HidDevice.h"
#include "utils/Logger.h"

#include <chrono>
#include <thread>

namespace mpgd {

DisplayThread::DisplayThread(SharedState& state, usb::HidDevice& device,
                             DisplayUpdater& updater, StateReader& stateReader,
                             int periodMs)
    : state_(state), device_(device), updater_(updater),
      stateReader_(stateReader), periodMs_(periodMs) {}

void DisplayThread::run() {
    logInfo("display thread started (period=%dms)", periodMs_);
    bool wasOpen = false;
    while (!state_.shutdown.load()) {
        bool isOpen = device_.isOpen();
        if (isOpen && !wasOpen) {
            // FR-01.3: initial "hello" frame on (re)connect.
            logInfo("display: sending hello frame");
        }
        if (isOpen) {
            // Refresh machine state (positions, e-stop, idle) before rendering.
            stateReader_.read();
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
        std::this_thread::sleep_for(std::chrono::milliseconds(periodMs_));
    }
    logInfo("display thread stopped");
}

} // namespace mpgd
