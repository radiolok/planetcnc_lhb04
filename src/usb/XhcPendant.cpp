#include "usb/XhcPendant.h"

#include "logic/ButtonQueue.h"
#include "utils/Logger.h"

namespace mpgd {

XhcPendant::XhcPendant(SharedState& state, ButtonQueue& buttons, const PollingConfig& polling,
                       bool verifyChecksum)
    : state_(state), buttons_(buttons), polling_(polling), verifyChecksum_(verifyChecksum) {}

bool XhcPendant::process(const uint8_t* data, size_t len, Clock::time_point now) {
    usb::ParsedInput p = usb::PacketParser::parseInput(data, len);
    if (p.reportId != xhc::kInputReportId) {
        logDebug("pendant: unexpected report id 0x%02X", p.reportId);
        return false;
    }
    if (!p.checksumOk) {
        // The reference driver (xhc-hb04.cc) does not verify a checksum; this
        // is an optional defensive check enabled via config.
        logDebug("pendant: checksum mismatch (got 0x%02X, expected 0x%02X)%s", p.checksumByte,
                 p.expectedChecksum, verifyChecksum_ ? "; report dropped" : "");
        if (verifyChecksum_) return false;
    }

    // Detect the "sleeping" state (all-zero fields, xhc-hb04.cc behaviour).
    bool sleeping = (p.button1 == 0 && p.button2 == 0 && p.axisCode == 0 && p.jogDelta == 0);

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

    onButton(p.button1, now);
    return true;
}

void XhcPendant::onButton(uint8_t code, Clock::time_point now) {
    rawButton_ = code; // always track the latest level
    tick(now);
}

void XhcPendant::tick(Clock::time_point now) {
    if (rawButton_ == committedButton_) return;
    if (haveEdge_) {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEdge_).count();
        // Debounce (SAFE-04): hold the change until the window has passed.
        if (elapsed < polling_.buttonDebounceMs) return;
    }
    commit(rawButton_, now);
}

void XhcPendant::commit(uint8_t code, Clock::time_point now) {
    const uint8_t previous = committedButton_;
    committedButton_ = code;
    lastEdge_ = now;
    haveEdge_ = true;

    if (code != 0) {
        const std::string name = xhc::buttonNameOrHex(code);
        logInfo("pendant: button '%s' (0x%02X) pressed", name.c_str(), code);
        if (!buttons_.push(name)) {
            logWarn("pendant: button queue full; '%s' dropped", name.c_str());
        }
    } else if (previous != 0) {
        logDebug("pendant: button '%s' released", xhc::buttonNameOrHex(previous).c_str());
    }
}

} // namespace mpgd
