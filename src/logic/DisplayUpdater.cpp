#include "logic/DisplayUpdater.h"

#include "config/ConfigManager.h"
#include "planetcnc/ITngApi.h"
#include "usb/PacketParser.h"

#include <cmath>

namespace mpgd {

DisplayUpdater::DisplayUpdater(ITngApi& api, SharedState& state,
                               const Config& cfg)
    : api_(api), state_(state), cfg_(cfg) {}

bool DisplayUpdater::build(Frame& out) {
    bool axisOff;
    int axis;
    double stepSize;
    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        axisOff = state_.axisOff();
        axis = state_.selectedAxis();
        stepSize = state_.stepSize;
    }

    // FR-04.5: skip display updates while the axis rotary is OFF (unless the
    // operator explicitly wants continuous display traffic).
    if (axisOff && !cfg_.polling.displayAlways) {
        return false;
    }

    // Read positions under lock so the three coordinates are consistent.
    double wx, wy, wz, mx, my, mz;
    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        wx = state_.machine.workX;
        wy = state_.machine.workY;
        wz = state_.machine.workZ;
        mx = state_.machine.motorX;
        my = state_.machine.motorY;
        mz = state_.machine.motorZ;
    }

    usb::DisplayData d;
    // Line 1 shows the selected axis (X or A), matching xhc-hb04.cc.
    if (axis == 3) {
        // A axis occupies the first line when selected.
        d.line1 = 0.0;  // A work position is not tracked in v1 (X/Y/Z only)
        d.machine1 = 0.0;
    } else {
        d.line1 = wx;
        d.machine1 = mx;
    }
    d.line2 = wy;
    d.line3 = wz;
    d.machine2 = my;
    d.machine3 = mz;

    d.feedOverride = api_.getParam(cfg_.jogging.feedOverrideParam);
    d.spindleOverride = api_.getParam(cfg_.jogging.spindleOverrideParam);
    d.feedValue = api_.infoSpeed();
    d.spindleRps = api_.infoSpindle();
    d.stepsize = static_cast<int>(llround(stepSize * 1000.0));
    d.inchIcon = false;
    d.aAxisActive = (axis == 3);

    uint8_t payload[xhc::kDisplayBufSize];
    usb::PacketParser::buildDisplayPayload(d, payload);

    // Split the 42-byte payload into 6 x 8-byte reports, each prefixed with
    // the output report ID (0x06).
    for (size_t r = 0; r < kReportsCount; ++r) {
        Report& report = out[r];
        report[0] = xhc::kOutputReportId;
        for (size_t i = 0; i < 7; ++i) {
            report[i + 1] = payload[r * 7 + i];
        }
    }

    return true;
}

} // namespace mpgd
