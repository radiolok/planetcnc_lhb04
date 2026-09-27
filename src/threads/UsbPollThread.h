#pragma once

#include "config/ConfigManager.h"
#include "logic/SharedState.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mpgd {

class XhcPendant;

namespace usb {
class HidDevice;
}

// The pendant connection as seen by the poll thread. The real implementation
// wraps the read and write HID devices; tests supply a fake to drive the
// reconnect path without hardware.
class IPendantLink {
public:
    IPendantLink() = default;
    virtual ~IPendantLink() = default;
    IPendantLink(const IPendantLink&) = delete;
    IPendantLink& operator=(const IPendantLink&) = delete;
    IPendantLink(IPendantLink&&) = delete;
    IPendantLink& operator=(IPendantLink&&) = delete;

    virtual bool isOpen() const = 0;
    // Opens the connection. Returns false with `error` set on failure.
    virtual bool open(std::string& error) = 0;
    virtual void close() = 0;
    // Reads one input report. Returns bytes read, 0 on timeout, <0 on error.
    virtual int read(uint8_t* data, size_t length, int timeoutMs) = 0;
    // Device description for the "connected" log line.
    virtual std::string describe() const = 0;
};

// Reads HID input reports (10 ms cadence), feeds them into the pendant state
// machine and handles USB reconnection (F-07). Owns opening of both the read
// (input) and write (display) HID collections.
class UsbPollThread {
public:
    UsbPollThread(SharedState& state, usb::HidDevice& readDevice, usb::HidDevice& writeDevice,
                  XhcPendant& pendant, const DeviceConfig& deviceCfg, const PollingConfig& polling,
                  bool sniff);
    UsbPollThread(SharedState& state, IPendantLink& link, XhcPendant& pendant,
                  const PollingConfig& polling, bool sniff);
    ~UsbPollThread();

    UsbPollThread(const UsbPollThread&) = delete;
    UsbPollThread& operator=(const UsbPollThread&) = delete;
    UsbPollThread(UsbPollThread&&) = delete;
    UsbPollThread& operator=(UsbPollThread&&) = delete;

    // Blocking loop until SharedState::shutdown is set.
    void run();

    // One loop iteration: connect if needed, then read and process one
    // report. Sleeps for the reconnect interval after a failed open or a read
    // error. run() calls this in a loop; tests call it directly.
    void pollOnce();

private:
    int pollPeriodMs() const;
    int reconnectMs() const;
    void markDisconnected();

    std::unique_ptr<IPendantLink> ownedLink_;
    SharedState& state_;
    IPendantLink& link_;
    XhcPendant& pendant_;
    const PollingConfig& polling_;
    bool sniff_;
};

} // namespace mpgd
