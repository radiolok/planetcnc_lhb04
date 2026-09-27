#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct hid_device_; // forward declaration of the opaque hidapi type

namespace mpgd::usb {

class HidDevice;

// One enumerated HID device node. A single physical pendant can expose
// several nodes on Windows (one per HID top-level collection), so callers
// must enumerate and pick the right node for input reads vs feature-report
// (display) writes.
struct HidDeviceInfo {
    std::string path;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    std::string manufacturer;
    std::string product;
    int interfaceNumber = -1;
};

// Enumerates all HID nodes matching any of the given (vendorId, productId).
std::vector<HidDeviceInfo> enumerateDevices(uint16_t vendorId,
                                            const std::vector<uint16_t>& productIds);

// Opens the pendant's read (input) and write (display) HID collections. On
// Windows the input report (0x04) and the LCD feature report (0x06) live in
// separate device nodes, so each node is probed with a feature-report write
// to find the write-capable one. On Linux/macOS a single node may serve both.
// Returns false with `error` set when no matching device can be opened.
bool openReadWrite(uint16_t vendorId, const std::vector<uint16_t>& productIds, HidDevice& readDev,
                   HidDevice& writeDev, std::string& error);

// Thin RAII wrapper around hidapi. Owns one open HID device.
//
// Thread-safe: every member serializes on an internal mutex, so one thread may
// close/reopen the device (USB poll thread on reconnect) while another is
// writing to it (display thread) without touching a freed hidapi handle.
class HidDevice {
public:
    HidDevice() = default;
    ~HidDevice();

    HidDevice(const HidDevice&) = delete;
    HidDevice& operator=(const HidDevice&) = delete;
    HidDevice(HidDevice&& other) noexcept;
    HidDevice& operator=(HidDevice&& other) noexcept;

    // Opens a specific device node by its enumeration path. Use
    // enumerateDevices() or openReadWrite() to find the node.
    bool openPath(const std::string& path);

    // Closes the device and marks it disconnected.
    void close();

    bool isOpen() const;

    // Reads one input report (blocking up to `timeoutMs`). Returns the number
    // of bytes read (0 on timeout, negative on error).
    int read(uint8_t* data, size_t length, int timeoutMs);

    // Writes one output report. Returns bytes written or negative on error.
    int write(const uint8_t* data, size_t length);

    // Sends a feature report (HID SET_REPORT). The LHB04 LCD is updated via
    // feature reports, not interrupt OUT writes.
    int sendFeatureReport(const uint8_t* data, size_t length);

    // Returns the manufacturer/product strings of the currently open device.
    std::string manufacturer() const;
    std::string product() const;

    // Last transport error from hidapi (empty when none). Useful for
    // diagnosing why a write/feature-report failed.
    std::string lastError() const;

    // Probes the length of a feature report via GET_REPORT. Returns the total
    // length including the report ID byte, or negative when the device does
    // not support GET_REPORT.
    int getFeatureReportLength(uint8_t reportId);

private:
    void closeLocked();

    mutable std::mutex mtx_;
    hid_device_* device_ = nullptr;
    std::string manufacturer_;
    std::string product_;
};

} // namespace mpgd::usb
