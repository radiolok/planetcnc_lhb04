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
        off += static_cast<size_t>(
            std::snprintf(line + off, sizeof(line) - off, "%02X ", data[i]));
    }
    logInfo("raw[%zu]: %s", len, line);
}

} // namespace

UsbPollThread::UsbPollThread(SharedState& state, usb::HidDevice& readDevice,
                             usb::HidDevice& writeDevice, XhcPendant& pendant,
                             const DeviceConfig& deviceCfg,
                             const PollingConfig& polling, bool sniff)
    : state_(state), readDevice_(readDevice), writeDevice_(writeDevice),
      pendant_(pendant), deviceCfg_(deviceCfg), polling_(polling), sniff_(sniff) {}

int UsbPollThread::pollPeriodMs() const {
    int hz = polling_.usbHz > 0 ? polling_.usbHz : 100;
    return 1000 / hz;
}

void UsbPollThread::run() {
    const int periodMs = pollPeriodMs();
    const int reconnectMs = polling_.reconnectMs > 0 ? polling_.reconnectMs : 2000;

    logInfo("usb poll thread started (period=%dms, reconnect=%dms)",
            periodMs, reconnectMs);

    while (!state_.shutdown.load()) {
        if (!readDevice_.isOpen() || !writeDevice_.isOpen()) {
            std::string err;
            if (usb::openReadWrite(deviceCfg_.vendorId, deviceCfg_.productIds,
                                   readDevice_, writeDevice_, err)) {
                logInfo("pendant connected: %s %s",
                        readDevice_.manufacturer().c_str(), readDevice_.product().c_str());
                {
                    std::lock_guard<std::mutex> lk(state_.mutex);
                    state_.pendant.connected = true;
                    state_.pendant.sleeping = false;
                }
            } else {
                {
                    std::lock_guard<std::mutex> lk(state_.mutex);
                    state_.pendant.connected = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(reconnectMs));
                continue;
            }
        }

        uint8_t buf[xhc::kInputPacketSize] = {0};
        int r = readDevice_.read(buf, sizeof(buf), periodMs);

        if (r >= static_cast<int>(xhc::kInputPacketSizeMin)) {
            if (sniff_) hexdump(buf, static_cast<size_t>(r));
            pendant_.process(buf, static_cast<size_t>(r));
        } else if (r < 0) {
            logWarn("pendant read error (%d), reconnecting", r);
            readDevice_.close();
            writeDevice_.close();
            {
                std::lock_guard<std::mutex> lk(state_.mutex);
                state_.pendant.connected = false;
            }
            state_.pendant.jogCounts.store(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnectMs));
        }
        // r == 0: read timeout, no data — loop again.
    }

    readDevice_.close();
    writeDevice_.close();
    logInfo("usb poll thread stopped");
}

} // namespace mpgd
