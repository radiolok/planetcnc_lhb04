#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct hid_device_; // forward declaration of the opaque hidapi type

namespace mpgd::usb {

// Thin RAII wrapper around hidapi. Owns one open HID device.
class HidDevice {
public:
    HidDevice() = default;
    ~HidDevice();

    HidDevice(const HidDevice&) = delete;
    HidDevice& operator=(const HidDevice&) = delete;
    HidDevice(HidDevice&& other) noexcept;
    HidDevice& operator=(HidDevice&& other) noexcept;

    // Opens the first device matching any of the given (vendorId, productId)
    // pairs. Returns false when no matching device is present.
    bool open(uint16_t vendorId, const std::vector<uint16_t>& productIds,
              std::string& error);

    // Closes the device and marks it disconnected.
    void close();

    bool isOpen() const { return device_ != nullptr; }

    // Reads one input report (blocking up to `timeoutMs`). Returns the number
    // of bytes read (0 on timeout, negative on error).
    int read(uint8_t* data, size_t length, int timeoutMs);

    // Writes one output report. Returns bytes written or negative on error.
    int write(const uint8_t* data, size_t length);

    // Returns the manufacturer/product strings of the currently open device.
    std::string manufacturer() const { return manufacturer_; }
    std::string product() const { return product_; }

private:
    hid_device_* device_ = nullptr;
    std::string manufacturer_;
    std::string product_;
};

} // namespace mpgd::usb
