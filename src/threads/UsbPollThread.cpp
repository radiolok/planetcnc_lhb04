#include "threads/UsbPollThread.h"

#include "usb/HidDevice.h"
#include "usb/XhcPendant.h"
#include "utils/Logger.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace mpgd {

namespace {

void hexdump(const uint8_t* data, size_t len) {
    char line[64];
    size_t off = 0;
    for (size_t i = 0; i < len && off < sizeof(line) - 4; ++i) {
        off += static_cast<size_t>(std::snprintf(line + off, sizeof(line) - off, "%02X ", data[i]));
    }
    logInfo("raw[%zu]: %s", len, line);
}

// The real pendant: separate HID nodes for input reads and display writes.
class HidPendantLink : public IPendantLink {
public:
    HidPendantLink(usb::HidDevice& readDevice, usb::HidDevice& writeDevice,
                   const DeviceConfig& deviceCfg)
        : readDevice_(readDevice), writeDevice_(writeDevice), deviceCfg_(deviceCfg) {}

    bool isOpen() const override { return readDevice_.isOpen() && writeDevice_.isOpen(); }
    bool open(std::string& error) override {
        return usb::openReadWrite(deviceCfg_.vendorId, deviceCfg_.productIds, readDevice_,
                                  writeDevice_, error);
    }
    void close() override {
        readDevice_.close();
        writeDevice_.close();
    }
    int read(uint8_t* data, size_t length, int timeoutMs) override {
        return readDevice_.read(data, length, timeoutMs);
    }
    std::string describe() const override {
        return readDevice_.manufacturer() + " " + readDevice_.product();
    }

private:
    usb::HidDevice& readDevice_;
    usb::HidDevice& writeDevice_;
    const DeviceConfig& deviceCfg_;
};

} // namespace

UsbPollThread::UsbPollThread(SharedState& state, usb::HidDevice& readDevice,
                             usb::HidDevice& writeDevice, XhcPendant& pendant,
                             const DeviceConfig& deviceCfg, const PollingConfig& polling,
                             bool sniff)
    : ownedLink_(std::make_unique<HidPendantLink>(readDevice, writeDevice, deviceCfg)),
      state_(state),
      link_(*ownedLink_),
      pendant_(pendant),
      polling_(polling),
      sniff_(sniff) {}

UsbPollThread::UsbPollThread(SharedState& state, IPendantLink& link, XhcPendant& pendant,
                             const PollingConfig& polling, bool sniff)
    : state_(state), link_(link), pendant_(pendant), polling_(polling), sniff_(sniff) {}

UsbPollThread::~UsbPollThread() = default;

int UsbPollThread::pollPeriodMs() const {
    int hz = polling_.usbHz > 0 ? polling_.usbHz : 100;
    return 1000 / hz;
}

int UsbPollThread::reconnectMs() const {
    return polling_.reconnectMs > 0 ? polling_.reconnectMs : 2000;
}

void UsbPollThread::markDisconnected() {
    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        state_.pendant.connected = false;
    }
    state_.pendant.jogCounts.store(0);
}

void UsbPollThread::run() {
    logInfo("usb poll thread started (period=%dms, reconnect=%dms)", pollPeriodMs(), reconnectMs());

    while (!state_.shutdown.load()) {
        pollOnce();
    }

    link_.close();
    logInfo("usb poll thread stopped");
}

void UsbPollThread::pollOnce() {
    if (!link_.isOpen()) {
        std::string err;
        if (!link_.open(err)) {
            markDisconnected();
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnectMs()));
            return;
        }
        logInfo("pendant connected: %s", link_.describe().c_str());
        std::lock_guard<std::mutex> lk(state_.mutex);
        state_.pendant.connected = true;
        state_.pendant.sleeping = false;
    }

    uint8_t buf[xhc::kInputPacketSize] = {0};
    int r = link_.read(buf, sizeof(buf), pollPeriodMs());

    if (r >= static_cast<int>(xhc::kInputPacketSizeMin)) {
        if (sniff_) hexdump(buf, static_cast<size_t>(r));
        pendant_.process(buf, static_cast<size_t>(r));
    } else if (r >= 0) {
        // Timeout or short report: let a debounced button change that is
        // due be committed even though no new report arrived.
        pendant_.tick();
    } else {
        logWarn("pendant read error (%d), reconnecting", r);
        link_.close();
        markDisconnected();
        std::this_thread::sleep_for(std::chrono::milliseconds(reconnectMs()));
    }
}

} // namespace mpgd
