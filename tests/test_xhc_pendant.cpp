#include "test_framework.h"
#include "logic/ButtonQueue.h"
#include "logic/SharedState.h"
#include "usb/XhcPendant.h"

#include <chrono>
#include <string>

using namespace mpgd;
using namespace std::chrono_literals;

namespace {

using Clock = XhcPendant::Clock;

// 8-byte input report with a valid checksum.
struct Report {
    uint8_t b[xhc::kInputPacketSize] = {0};
    Report(uint8_t button, uint8_t axis = xhc::kAxisX, int8_t jog = 0) {
        b[xhc::kOffsetReportId] = xhc::kInputReportId;
        b[xhc::kOffsetButton1] = button;
        b[xhc::kOffsetAxis] = axis;
        b[xhc::kOffsetJogDelta] = static_cast<uint8_t>(jog);
        b[xhc::kOffsetChecksum] = usb::PacketParser::computeChecksum(b, sizeof(b));
    }
};

const uint8_t kHome = static_cast<uint8_t>(xhc::Button::Home);
const uint8_t kStop = static_cast<uint8_t>(xhc::Button::Stop);

struct Fixture {
    SharedState state;
    ButtonQueue queue;
    PollingConfig polling;
    XhcPendant pendant{state, queue, polling, true};
    Clock::time_point t0 = Clock::now();

    void send(uint8_t button, std::chrono::milliseconds at) {
        Report r(button);
        pendant.process(r.b, sizeof(r.b), t0 + at);
    }
    std::string pop() {
        auto n = queue.popFor(0ms);
        return n ? *n : std::string("<empty>");
    }
};

} // namespace

// A press is queued at once, not executed on the polling thread.
static void test_press_is_queued() {
    Fixture f;
    f.send(kHome, 0ms);
    CHECK_EQ(f.queue.size(), 1u);
    CHECK_EQ(f.pop(), std::string("home"));
}

// Bounce inside the debounce window does not produce a second press.
static void test_bounce_suppressed() {
    Fixture f;
    f.send(kHome, 0ms);
    f.send(0, 5ms);
    f.send(kHome, 10ms);
    f.pendant.tick(f.t0 + 200ms);
    CHECK_EQ(f.queue.size(), 1u);
}

// Release then a new press inside the window: the press is delayed until the
// window ends, not lost (review item 18).
static void test_press_within_window_not_lost() {
    Fixture f;
    f.send(kHome, 0ms);
    CHECK_EQ(f.pop(), std::string("home"));
    f.send(0, 100ms);                    // release, commits (window passed)
    f.send(kStop, 120ms);                // within 50 ms of the release
    CHECK_EQ(f.queue.size(), 0u);        // held off for now
    f.pendant.tick(f.t0 + 140ms);
    CHECK_EQ(f.queue.size(), 0u);
    f.pendant.tick(f.t0 + 151ms);        // no new report, only a poll tick
    CHECK_EQ(f.pop(), std::string("stop"));
}

// Held button: repeated identical reports never re-trigger.
static void test_held_button_single_press() {
    Fixture f;
    for (int i = 0; i < 20; ++i) f.send(kHome, std::chrono::milliseconds(i * 10));
    CHECK_EQ(f.queue.size(), 1u);
}

// A full queue drops further presses instead of blocking the poll thread.
static void test_queue_bounded() {
    ButtonQueue q;
    for (size_t i = 0; i < ButtonQueue::kCapacity; ++i) CHECK(q.push("home"));
    CHECK(!q.push("stop"));
    CHECK_EQ(q.size(), ButtonQueue::kCapacity);
}

// Wheel counts and axis still reach the shared state.
static void test_state_published() {
    Fixture f;
    Report r(0, xhc::kAxisY, 3);
    f.pendant.process(r.b, sizeof(r.b), f.t0);
    CHECK_EQ(f.state.snapshot().selectedAxis(), 1);
    CHECK_EQ(f.state.pendant.jogCounts.load(), 3);
}

// All-zero fields mean the pendant is asleep; any activity wakes it.
static void test_sleeping_state() {
    Fixture f;
    Report asleep(0, 0, 0);
    CHECK(f.pendant.process(asleep.b, sizeof(asleep.b), f.t0));
    CHECK(f.state.snapshot().pendantSleeping);
    CHECK(f.state.snapshot().pendantConnected);

    Report awake(0, xhc::kAxisX, 0);
    CHECK(f.pendant.process(awake.b, sizeof(awake.b), f.t0 + 10ms));
    CHECK(!f.state.snapshot().pendantSleeping);
}

// Wheel deltas add up in both directions until the jog thread takes them.
static void test_jog_counts_accumulate() {
    Fixture f;
    const int8_t deltas[] = {1, 5, -2, 127, -128, 0};
    int expected = 0;
    auto at = 0ms;
    for (int8_t d : deltas) {
        Report r(0, xhc::kAxisZ, d);
        f.pendant.process(r.b, sizeof(r.b), f.t0 + at);
        expected += d;
        at += 10ms;
    }
    CHECK_EQ(f.state.pendant.jogCounts.load(), expected);

    // The jog thread drains with exchange(0); new counts start from zero.
    CHECK_EQ(f.state.pendant.jogCounts.exchange(0), expected);
    Report r(0, xhc::kAxisZ, -3);
    f.pendant.process(r.b, sizeof(r.b), f.t0 + at);
    CHECK_EQ(f.state.pendant.jogCounts.load(), -3);
}

// A report with a bad checksum is dropped when verification is on: no counts,
// no button press.
static void test_bad_checksum_dropped() {
    Fixture f;
    Report r(kHome, xhc::kAxisX, 4);
    r.b[xhc::kOffsetChecksum] ^= 0xFF;
    CHECK(!f.pendant.process(r.b, sizeof(r.b), f.t0));
    CHECK_EQ(f.state.pendant.jogCounts.load(), 0);
    CHECK_EQ(f.pop(), std::string("<empty>"));
}

int main() {
    test_press_is_queued();
    test_bounce_suppressed();
    test_press_within_window_not_lost();
    test_held_button_single_press();
    test_queue_bounded();
    test_state_published();
    test_sleeping_state();
    test_jog_counts_accumulate();
    test_bad_checksum_dropped();
    return tfw::summary("test_xhc_pendant");
}
