#include "usb/HidDevice.h"

#include <hidapi.h>

#include <cwchar>
#include <string>
#include <utility>

namespace mpgd::usb {

namespace {

std::string narrow(const wchar_t* w) {
    std::string out;
    if (!w) return out;
    for (; *w; ++w) out.push_back(static_cast<char>(*w));
    return out;
}

} // namespace

HidDevice::~HidDevice() {
    close();
}

HidDevice::HidDevice(HidDevice&& other) noexcept
    : device_(other.device_),
      manufacturer_(std::move(other.manufacturer_)),
      product_(std::move(other.product_)) {
    other.device_ = nullptr;
}

HidDevice& HidDevice::operator=(HidDevice&& other) noexcept {
    if (this != &other) {
        close();
        device_ = other.device_;
        manufacturer_ = std::move(other.manufacturer_);
        product_ = std::move(other.product_);
        other.device_ = nullptr;
    }
    return *this;
}

bool HidDevice::open(uint16_t vendorId, const std::vector<uint16_t>& productIds,
                     std::string& error) {
    close();

    for (uint16_t pid : productIds) {
        hid_device_* dev = hid_open(vendorId, pid, nullptr);
        if (dev) {
            device_ = dev;
            wchar_t buf[256] = {0};
            if (hid_get_manufacturer_string(dev, buf, sizeof(buf) / sizeof(buf[0])) == 0) {
                manufacturer_ = narrow(buf);
            }
            if (hid_get_product_string(dev, buf, sizeof(buf) / sizeof(buf[0])) == 0) {
                product_ = narrow(buf);
            }
            return true;
        }
    }

    error = "no XHC LHB04 device found (vid=" + std::to_string(vendorId) + ")";
    return false;
}

void HidDevice::close() {
    if (device_) {
        hid_close(device_);
        device_ = nullptr;
        manufacturer_.clear();
        product_.clear();
    }
}

int HidDevice::read(uint8_t* data, size_t length, int timeoutMs) {
    if (!device_) return -1;
    return hid_read_timeout(device_, data, static_cast<size_t>(length), timeoutMs);
}

int HidDevice::write(const uint8_t* data, size_t length) {
    if (!device_) return -1;
    return hid_write(device_, data, static_cast<size_t>(length));
}

} // namespace mpgd::usb
