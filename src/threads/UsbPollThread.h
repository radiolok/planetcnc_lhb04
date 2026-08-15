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
// machine and handles USB reconnection (F-07).
class UsbPollThread {
public:
    UsbPollThread(SharedState& state, usb::HidDevice& device, XhcPendant& pendant,
                  const DeviceConfig& deviceCfg, const PollingConfig& polling,
                  bool sniff);

    // Blocking loop until SharedState::shutdown is set.
    void run();

private:
    int pollPeriodMs() const;

    SharedState& state_;
    usb::HidDevice& device_;
    XhcPendant& pendant_;
    const DeviceConfig& deviceCfg_;
    const PollingConfig& polling_;
    bool sniff_;
};

} // namespace mpgd
