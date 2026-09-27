#pragma once

#include "config/ConfigManager.h"
#include "logic/SharedState.h"
#include "usb/PacketParser.h"

#include <chrono>
#include <cstdint>

namespace mpgd {

class ButtonQueue;

// High-level pendant state machine: parses raw reports, performs button
// edge-detection/debounce, accumulates MPG wheel counts and publishes
// everything into the shared state. Button presses are queued for the button
// thread rather than executed here.
class XhcPendant {
public:
    using Clock = std::chrono::steady_clock;

    XhcPendant(SharedState& state, ButtonQueue& buttons,
               const PollingConfig& polling, bool verifyChecksum);

    // Processes one raw 8-byte input report. Returns false for malformed
    // packets (wrong report id / short buffer / checksum mismatch when
    // verification is enabled).
    bool process(const uint8_t* data, size_t len) { return process(data, len, Clock::now()); }
    bool process(const uint8_t* data, size_t len, Clock::time_point now);

    // Commits a button level that changed during the debounce window once the
    // window has passed. Call on every poll iteration, including read
    // timeouts: the pendant may send no further report after the change.
    void tick() { tick(Clock::now()); }
    void tick(Clock::time_point now);

private:
    void onButton(uint8_t code, Clock::time_point now);
    void commit(uint8_t code, Clock::time_point now);

    SharedState& state_;
    ButtonQueue& buttons_;
    const PollingConfig& polling_;
    bool verifyChecksum_;

    // Debounce: the first edge is acted on at once, then further edges are
    // held off for button_debounce_ms. The latest raw level is kept, and if it
    // still differs from the committed level when the hold-off ends it is
    // committed then, so a real press inside the window is delayed, not lost.
    uint8_t rawButton_ = 0;        // latest level reported by the pendant
    uint8_t committedButton_ = 0;  // level last acted on
    Clock::time_point lastEdge_{};
    bool haveEdge_ = false;
};

} // namespace mpgd
