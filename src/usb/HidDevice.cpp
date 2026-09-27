#include "usb/HidDevice.h"

#include "utils/Utf8.h"

#include <hidapi.h>

#include <cwchar>
#include <mutex>
#include <string>
#include <utility>

namespace mpgd::usb {

HidDevice::~HidDevice() {
    close();
}

HidDevice::HidDevice(HidDevice&& other) noexcept {
    std::lock_guard<std::mutex> lk(other.mtx_);
    device_ = other.device_;
    manufacturer_ = std::move(other.manufacturer_);
    product_ = std::move(other.product_);
    other.device_ = nullptr;
}

HidDevice& HidDevice::operator=(HidDevice&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lk(mtx_, other.mtx_);
        closeLocked();
        device_ = other.device_;
        manufacturer_ = std::move(other.manufacturer_);
        product_ = std::move(other.product_);
        other.device_ = nullptr;
    }
    return *this;
}

bool HidDevice::openPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx_);
    closeLocked();
    hid_device_* dev = hid_open_path(path.c_str());
    if (!dev) return false;
    device_ = dev;
    wchar_t buf[256] = {0};
    if (hid_get_manufacturer_string(dev, buf, sizeof(buf) / sizeof(buf[0])) == 0) {
        manufacturer_ = utf8FromWide(buf);
    }
    if (hid_get_product_string(dev, buf, sizeof(buf) / sizeof(buf[0])) == 0) {
        product_ = utf8FromWide(buf);
    }
    return true;
}

std::vector<HidDeviceInfo> enumerateDevices(uint16_t vendorId,
                                            const std::vector<uint16_t>& productIds) {
    std::vector<HidDeviceInfo> out;
    hid_device_info* devs = hid_enumerate(vendorId, 0x0);
    for (hid_device_info* d = devs; d; d = d->next) {
        bool match = productIds.empty();
        for (uint16_t pid : productIds) {
            if (d->product_id == pid) {
                match = true;
                break;
            }
        }
        if (!match) continue;

        HidDeviceInfo info;
        info.path = d->path ? d->path : "";
        info.vendorId = d->vendor_id;
        info.productId = d->product_id;
        info.manufacturer = utf8FromWide(d->manufacturer_string);
        info.product = utf8FromWide(d->product_string);
        info.interfaceNumber = d->interface_number;
        out.push_back(info);
    }
    hid_free_enumeration(devs);
    return out;
}

bool openReadWrite(uint16_t vendorId, const std::vector<uint16_t>& productIds,
                   HidDevice& readDev, HidDevice& writeDev, std::string& error) {
    const std::vector<HidDeviceInfo> devs = enumerateDevices(vendorId, productIds);
    if (devs.empty()) {
        error = "no XHC LHB04 device found";
        return false;
    }

    if (devs.size() == 1) {
        if (!readDev.openPath(devs[0].path) || !writeDev.openPath(devs[0].path)) {
            error = "open failed";
            return false;
        }
        return true;
    }

    HidDeviceInfo readInfo;
    HidDeviceInfo writeInfo;
    bool haveRead = false;
    bool haveWrite = false;
    for (const auto& info : devs) {
        HidDevice probe;
        if (!probe.openPath(info.path)) continue;
        uint8_t rep[8] = {0x06, 0, 0, 0, 0, 0, 0, 0};
        const int wr = probe.sendFeatureReport(rep, sizeof(rep));
        if (wr >= 0) {
            if (!haveWrite) { writeInfo = info; haveWrite = true; }
        } else {
            if (!haveRead) { readInfo = info; haveRead = true; }
        }
    }

    if (haveWrite && haveRead) {
        if (!readDev.openPath(readInfo.path) || !writeDev.openPath(writeInfo.path)) {
            error = "open read/write collections failed";
            return false;
        }
        return true;
    }

    // Could not separate roles; use the first node for both.
    if (!readDev.openPath(devs[0].path) || !writeDev.openPath(devs[0].path)) {
        error = "open failed";
        return false;
    }
    return true;
}

void HidDevice::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    closeLocked();
}

bool HidDevice::isOpen() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return device_ != nullptr;
}

std::string HidDevice::manufacturer() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return manufacturer_;
}

std::string HidDevice::product() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return product_;
}

void HidDevice::closeLocked() {
    if (device_) {
        hid_close(device_);
        device_ = nullptr;
        manufacturer_.clear();
        product_.clear();
    }
}

int HidDevice::read(uint8_t* data, size_t length, int timeoutMs) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!device_) return -1;
    return hid_read_timeout(device_, data, static_cast<size_t>(length), timeoutMs);
}

int HidDevice::write(const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!device_) return -1;
    return hid_write(device_, data, static_cast<size_t>(length));
}

int HidDevice::sendFeatureReport(const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!device_) return -1;
    return hid_send_feature_report(device_, data, static_cast<size_t>(length));
}

std::string HidDevice::lastError() const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!device_) return std::string();
    const wchar_t* e = hid_error(device_);
    return e ? utf8FromWide(e) : std::string();
}

int HidDevice::getFeatureReportLength(uint8_t reportId) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!device_) return -1;
    uint8_t buf[256] = {0};
    buf[0] = reportId;
    return hid_get_feature_report(device_, buf, sizeof(buf));
}

} // namespace mpgd::usb
