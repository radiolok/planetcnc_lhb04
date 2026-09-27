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
    const StateSnapshot snap = state_.snapshot();
    const bool axisOff = snap.axisOff();
    const int axis = snap.selectedAxis();
    const double stepSize = snap.stepSize;

    // FR-04.5: skip display updates while the axis rotary is OFF (unless the
    // operator explicitly wants continuous display traffic).
    if (axisOff && !cfg_.polling.displayAlways) {
        return false;
    }

    // Positions come from the same snapshot, so the three coordinates are
    // consistent.
    const double wx = snap.machine.workX;
    const double wy = snap.machine.workY;
    const double wz = snap.machine.workZ;
    const double mx = snap.machine.motorX;
    const double my = snap.machine.motorY;
    const double mz = snap.machine.motorZ;

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

    d.feedOverride = api_.getParam(cfg_.jogging.feedOverrideParam).value_or(0.0);
    d.spindleOverride = api_.getParam(cfg_.jogging.spindleOverrideParam).value_or(0.0);
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
