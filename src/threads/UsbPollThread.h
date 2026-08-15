#pragma once

#include "config/ConfigManager.h"
#include "logic/SharedState.h"

#include <cstdint>

namespace mpgd {

class XhcPendant;

namespace usb {
class HidDevice;
}

// Reads HID input reports (10 ms cadence), feeds them into the pendant state
// machine and handles USB reconnection (F-07). Owns opening of both the read
// (input) and write (display) HID collections.
class UsbPollThread {
public:
    UsbPollThread(SharedState& state, usb::HidDevice& readDevice,
                  usb::HidDevice& writeDevice, XhcPendant& pendant,
                  const DeviceConfig& deviceCfg, const PollingConfig& polling,
                  bool sniff);

    // Blocking loop until SharedState::shutdown is set.
    void run();

private:
    int pollPeriodMs() const;

    SharedState& state_;
    usb::HidDevice& readDevice_;
    usb::HidDevice& writeDevice_;
    XhcPendant& pendant_;
    const DeviceConfig& deviceCfg_;
    const PollingConfig& polling_;
    bool sniff_;
};

} // namespace mpgd
