#pragma once

#include "config/ConfigManager.h"
#include "logic/SharedState.h"
#include "usb/PacketParser.h"

#include <chrono>
#include <cstdint>

namespace mpgd {

class ButtonHandler;

// High-level pendant state machine: parses raw reports, performs button
// edge-detection/debounce, accumulates MPG wheel counts and publishes
// everything into the shared state.
class XhcPendant {
public:
    XhcPendant(SharedState& state, ButtonHandler& buttons,
               const PollingConfig& polling, bool verifyChecksum);

    // Processes one raw 8-byte input report. Returns false for malformed
    // packets (wrong report id / short buffer / checksum mismatch when
    // verification is enabled).
    bool process(const uint8_t* data, size_t len);

private:
    void onButton(uint8_t code);

    SharedState& state_;
    ButtonHandler& buttons_;
    const PollingConfig& polling_;
    bool verifyChecksum_;

    uint8_t lastButton_ = 0;
    std::chrono::steady_clock::time_point lastEdge_{};
};

} // namespace mpgd
