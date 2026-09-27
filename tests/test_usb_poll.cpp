#include "test_framework.h"
#include "logic/ButtonQueue.h"
#include "logic/SharedState.h"
#include "threads/UsbPollThread.h"
#include "usb/XhcPendant.h"

#include <deque>
#include <string>

using namespace mpgd;

namespace {

// Scripted pendant connection: open() results and read() results are taken
// from queues, so a test can play a connect / read / unplug / replug sequence.
class FakeLink : public IPendantLink {
public:
    struct ReadResult {
        int rc;                 // >0: a report of that size, 0: timeout, <0: error
        int8_t jog = 0;
    };

    std::deque<bool> openResults;
    std::deque<ReadResult> reads;
    bool opened = false;
    int openCalls = 0;
    int closeCalls = 0;

    bool isOpen() const override { return opened; }
    bool open(std::string& error) override {
        ++openCalls;
        bool ok = false;
        if (!openResults.empty()) {
            ok = openResults.front();
            openResults.pop_front();
        }
        if (!ok) error = "no device";
        opened = ok;
        return ok;
    }
    void close() override {
        ++closeCalls;
        opened = false;
    }
    int read(uint8_t* data, size_t length, int /*timeoutMs*/) override {
        if (reads.empty()) return 0;
        ReadResult r = reads.front();
        reads.pop_front();
        if (r.rc > 0) {
            for (size_t i = 0; i < length; ++i) data[i] = 0;
            data[xhc::kOffsetReportId] = xhc::kInputReportId;
            data[xhc::kOffsetAxis] = xhc::kAxisX;
            data[xhc::kOffsetJogDelta] = static_cast<uint8_t>(r.jog);
            data[xhc::kOffsetChecksum] =
                usb::PacketParser::computeChecksum(data, length);
        }
        return r.rc;
    }
    std::string describe() const override { return "fake pendant"; }
};

struct Fixture {
    SharedState state;
    ButtonQueue queue;
    PollingConfig polling;
    FakeLink link;
    XhcPendant pendant{state, queue, polling, true};
    UsbPollThread thread{state, link, pendant, polling, false};

    Fixture() { polling.reconnectMs = 1; }
    bool connected() const { return state.snapshot().pendantConnected; }
};

const int kReport = static_cast<int>(xhc::kInputPacketSize);

} // namespace

// No device: stays disconnected and keeps retrying.
static void test_retries_while_absent() {
    Fixture f;
    f.link.openResults = {false, false};
    f.thread.pollOnce();
    f.thread.pollOnce();
    CHECK_EQ(f.link.openCalls, 2);
    CHECK(!f.connected());
}

// Device appears: connected, and reports reach the pendant state machine.
static void test_connects_and_reads() {
    Fixture f;
    f.link.openResults = {false, true};
    f.link.reads = {{kReport, 4}};
    f.thread.pollOnce();
    CHECK(!f.connected());
    f.thread.pollOnce();
    CHECK(f.connected());
    CHECK_EQ(f.state.pendant.jogCounts.load(), 4);
}

// Read error (cable pulled): link closed, marked disconnected and unconsumed
// wheel counts dropped so they cannot jog the machine after a replug. The
// next iteration reconnects.
static void test_read_error_reconnects() {
    Fixture f;
    f.link.openResults = {true, true};
    f.link.reads = {{kReport, 7}, {-1}, {kReport, 2}};
    f.thread.pollOnce();
    CHECK_EQ(f.state.pendant.jogCounts.load(), 7);

    f.thread.pollOnce();
    CHECK(!f.link.opened);
    CHECK_EQ(f.link.closeCalls, 1);
    CHECK(!f.connected());
    CHECK_EQ(f.state.pendant.jogCounts.load(), 0);

    f.thread.pollOnce();
    CHECK_EQ(f.link.openCalls, 2);
    CHECK(f.connected());
    CHECK_EQ(f.state.pendant.jogCounts.load(), 2);
}

// A timeout is not a disconnect.
static void test_timeout_keeps_connection() {
    Fixture f;
    f.link.openResults = {true};
    f.link.reads = {{0}, {0}};
    f.thread.pollOnce();
    f.thread.pollOnce();
    CHECK(f.connected());
    CHECK_EQ(f.link.openCalls, 1);
    CHECK_EQ(f.link.closeCalls, 0);
}

// run() returns on shutdown and closes the link.
static void test_run_closes_on_shutdown() {
    Fixture f;
    f.link.opened = true;
    f.state.shutdown.store(true);
    f.thread.run();
    CHECK_EQ(f.link.closeCalls, 1);
}

int main() {
    test_retries_while_absent();
    test_connects_and_reads();
    test_read_error_reconnects();
    test_timeout_keeps_connection();
    test_run_closes_on_shutdown();
    return tfw::summary("test_usb_poll");
}
