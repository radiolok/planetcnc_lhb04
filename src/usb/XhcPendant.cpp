#include "usb/XhcPendant.h"

#include "logic/ButtonHandler.h"
#include "utils/Logger.h"

namespace mpgd {

XhcPendant::XhcPendant(SharedState& state, ButtonHandler& buttons,
                       const PollingConfig& polling)
    : state_(state), buttons_(buttons), polling_(polling) {}

bool XhcPendant::process(const uint8_t* data, size_t len) {
    usb::ParsedInput p = usb::PacketParser::parseInput(data, len);
    if (p.reportId != xhc::kInputReportId) {
        logDebug("pendant: unexpected report id 0x%02X", p.reportId);
        return false;
    }
    if (!p.checksumOk) {
        // The reference driver (xhc-hb04.cc) does not verify a checksum; this
        // is an optional defensive check enabled via config.
        logDebug("pendant: checksum mismatch (got 0x%02X, expected 0x%02X)",
                 p.checksumByte, p.expectedChecksum);
    }

    // Detect the "sleeping" state (all-zero fields, xhc-hb04.cc behaviour).
    bool sleeping = (p.button1 == 0 && p.button2 == 0 && p.axisCode == 0 &&
                     p.jogDelta == 0);

    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        state_.pendant.connected = true;
        state_.pendant.sleeping = sleeping;
        state_.pendant.lastJogDelta = p.jogDelta;
        state_.pendant.axisCode = p.axisCode;
    }

    // Accumulate wheel counts for the jog thread.
    if (p.jogDelta != 0) {
        state_.pendant.jogCounts.fetch_add(static_cast<int>(p.jogDelta));
    }

    onButton(p.button1);
    return true;
}

void XhcPendant::onButton(uint8_t code) {
    if (code == lastButton_) return;

    auto now = std::chrono::steady_clock::now();
    bool withinDebounce = false;
    if (lastEdge_.time_since_epoch().count() != 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - lastEdge_).count();
        withinDebounce = elapsed < polling_.buttonDebounceMs;
    }

    uint8_t previous = lastButton_;
    lastButton_ = code; // always track the latest level

    if (withinDebounce) {
        return; // debounce (SAFE-04): drop the transition but not the state
    }
    lastEdge_ = now;

    if (code != 0) {
        logInfo("pendant: button '%s' (0x%02X) pressed",
                xhc::buttonNameOrHex(code).c_str(), code);
        buttons_.onPress(xhc::buttonNameOrHex(code));
    } else if (previous != 0) {
        logDebug("pendant: button '%s' released",
                 xhc::buttonNameOrHex(previous).c_str());
    }
}

} // namespace mpgd
