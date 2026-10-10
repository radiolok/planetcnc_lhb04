#include "test_framework.h"
#include "usb/PacketParser.h"

#include <cstring>
#include <limits>

using namespace mpgd;
using namespace mpgd::usb;

static uint16_t readLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static void test_encode_coordinate_positive() {
    uint8_t buf[4] = {0};
    PacketParser::encodeCoordinate(12.345, buf);
    CHECK_EQ(readLE16(buf), 12);       // integer part (magnitude)
    CHECK_EQ(readLE16(buf + 2), 3450); // fraction x 10000, no sign bit
}

static void test_encode_coordinate_negative() {
    uint8_t buf[4] = {0};
    PacketParser::encodeCoordinate(-12.345, buf);
    CHECK_EQ(readLE16(buf), 12);
    CHECK_EQ(readLE16(buf + 2), static_cast<uint16_t>(0x8000 | 3450));
}

static void test_encode_coordinate_zero() {
    uint8_t buf[4] = {0};
    PacketParser::encodeCoordinate(0.0, buf);
    CHECK_EQ(readLE16(buf), 0);
    CHECK_EQ(readLE16(buf + 2), 0);
}

static void test_encode_coordinate_large() {
    uint8_t buf[4] = {0};
    PacketParser::encodeCoordinate(-9999.999, buf);
    CHECK_EQ(readLE16(buf), 9999);
    CHECK_EQ(readLE16(buf + 2), static_cast<uint16_t>(0x8000 | 9990));
}

// Out-of-range values saturate instead of wrapping (review item 20).
static void test_encode_coordinate_saturates() {
    uint8_t buf[4] = {0};
    PacketParser::encodeCoordinate(70000.0, buf);
    CHECK_EQ(readLE16(buf), 65535);
    CHECK_EQ(readLE16(buf + 2), 9999);
    PacketParser::encodeCoordinate(-1e12, buf);
    CHECK_EQ(readLE16(buf), 65535);
    CHECK_EQ(readLE16(buf + 2), static_cast<uint16_t>(0x8000 | 9999));
    PacketParser::encodeCoordinate(std::numeric_limits<double>::quiet_NaN(), buf);
    CHECK_EQ(readLE16(buf), 0);
    CHECK_EQ(readLE16(buf + 2), 0);
}

static void test_build_display_rates_saturate() {
    DisplayData d;
    d.feedValue = 1000.0;     // x60 = 60000, beyond int16
    d.spindleRps = -1000.0;   // x60 = -60000
    d.feedOverride = std::numeric_limits<double>::infinity();
    uint8_t payload[xhc::kDisplayBufSize];
    PacketParser::buildDisplayPayload(d, payload);
    CHECK_EQ(readLE16(payload + 27), 0);
    CHECK_EQ(readLE16(payload + 31), 32767);
    CHECK_EQ(readLE16(payload + 33), 0x8000);
}

static void test_build_display_payload() {
    DisplayData d;
    d.line1 = 12.345;
    d.line2 = -0.001;
    d.line3 = 0.0;
    d.machine1 = 1.0;
    d.machine2 = 2.0;
    d.machine3 = 3.0;
    d.feedOverride = 1.0;    // -> 100
    d.spindleOverride = 0.5; // -> 50
    d.feedValue = 50.0;      // -> 3000 units/min
    d.spindleRps = 200.0;    // -> 12000 RPM
    d.stepsize = 10;         // -> display code 0x03
    d.inchIcon = false;

    uint8_t payload[xhc::kDisplayBufSize];
    PacketParser::buildDisplayPayload(d, payload);

    // Header (xhc-hb04.cc: 0xFE 0xFD 0x0C).
    CHECK_EQ(payload[0], 0xFE);
    CHECK_EQ(payload[1], 0xFD);
    CHECK_EQ(payload[2], 0x0C);

    // Line 1 (work): +12.345.
    CHECK_EQ(readLE16(payload + 3), 12);
    CHECK_EQ(readLE16(payload + 5), 3450);
    // Line 2 (work): -0.001.
    CHECK_EQ(readLE16(payload + 7), 0);
    CHECK_EQ(readLE16(payload + 9), static_cast<uint16_t>(0x8000 | 10));

    // Feed override x100 at offset 27, spindle override x100 at 29.
    CHECK_EQ(readLE16(payload + 27), 100);
    CHECK_EQ(readLE16(payload + 29), 50);
    // Feed value x60 at offset 31, spindle rps x60 at 33.
    CHECK_EQ(readLE16(payload + 31), 3000);
    CHECK_EQ(readLE16(payload + 33), 12000);

    // Stepsize display code and flags.
    CHECK_EQ(payload[xhc::kDispStepsizeByte], xhc::kStepDisplay10);
    CHECK_EQ(payload[xhc::kDispFlagsByte], 0);
}

static void test_build_display_stepsizes() {
    struct Case {
        int step;
        uint8_t code;
    };
    Case cases[] = {{1, xhc::kStepDisplay1},       {5, xhc::kStepDisplay5},
                    {10, xhc::kStepDisplay10},     {100, xhc::kStepDisplay100},
                    {1000, xhc::kStepDisplay1000}, {7, xhc::kStepDisplay0}}; // unsupported -> 0
    for (const auto& c : cases) {
        DisplayData d;
        d.stepsize = c.step;
        uint8_t payload[xhc::kDisplayBufSize];
        PacketParser::buildDisplayPayload(d, payload);
        CHECK_EQ(payload[xhc::kDispStepsizeByte], c.code);
    }
}

static void test_build_display_inch_flag() {
    DisplayData d;
    d.inchIcon = true;
    uint8_t payload[xhc::kDisplayBufSize];
    PacketParser::buildDisplayPayload(d, payload);
    CHECK_EQ(payload[xhc::kDispFlagsByte], xhc::kFlagInchIcon);
}

int main() {
    test_encode_coordinate_positive();
    test_encode_coordinate_negative();
    test_encode_coordinate_zero();
    test_encode_coordinate_large();
    test_encode_coordinate_saturates();
    test_build_display_rates_saturate();
    test_build_display_payload();
    test_build_display_stepsizes();
    test_build_display_inch_flag();
    return tfw::summary("test_display_updater");
}
