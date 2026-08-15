#include "usb/PacketParser.h"

#include <cmath>

namespace mpgd::usb {

namespace {

void writeU16LE(uint8_t* out, uint16_t v) {
    out[0] = static_cast<uint8_t>(v & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void writeS16(uint8_t* out, int v) {
    writeU16LE(out, static_cast<uint16_t>(v));
}

uint8_t stepsizeDisplayCode(int stepsize) {
    switch (stepsize) {
        case    0: return xhc::kStepDisplay0;
        case    1: return xhc::kStepDisplay1;
        case    5: return xhc::kStepDisplay5;
        case   10: return xhc::kStepDisplay10;
        case   20: return xhc::kStepDisplay20;
        case   30: return xhc::kStepDisplay30;
        case   40: return xhc::kStepDisplay40;
        case   50: return xhc::kStepDisplay50;
        case  100: return xhc::kStepDisplay100;
        case  500: return xhc::kStepDisplay500;
        case 1000: return xhc::kStepDisplay1000;
        default:   return xhc::kStepDisplay0;
    }
}

} // namespace

ParsedInput PacketParser::parseInput(const uint8_t* data, size_t len) {
    ParsedInput p;
    if (len < xhc::kInputPacketSizeMin) {
        p.reportId = 0xFF;
        return p;
    }
    if (data[xhc::kOffsetReportId] != xhc::kInputReportId) {
        p.reportId = data[xhc::kOffsetReportId];
        return p;
    }
    p.reportId   = data[xhc::kOffsetReportId];
    p.button1    = data[xhc::kOffsetButton1];
    p.button2    = data[xhc::kOffsetButton2];
    p.axisCode   = data[xhc::kOffsetAxis];
    p.jogDelta   = static_cast<int8_t>(data[xhc::kOffsetJogDelta]);
    p.feedRotary = data[xhc::kOffsetFeed];

    // Seed and checksum only exist in the full 8-byte reference report.
    // 6-byte KTURT reports omit them, so leave checksumOk as the default
    // (true) and mark that no checksum verification was performed.
    if (len >= xhc::kInputPacketSize) {
        p.seed             = data[xhc::kOffsetSeed];
        p.checksumByte     = data[xhc::kOffsetChecksum];
        p.expectedChecksum = computeChecksum(data, len);
        p.checksumOk       = (p.checksumByte == p.expectedChecksum);
        p.hasChecksum      = true;
    }
    return p;
}

uint8_t PacketParser::computeChecksum(const uint8_t* data, size_t len) {
    // XOR of bytes [1..6]. The report ID (byte 0) and checksum byte (byte 7)
    // are excluded.
    if (len < xhc::kInputPacketSize) return 0;
    uint8_t c = 0;
    for (size_t i = 1; i <= 6; ++i) c ^= data[i];
    return c;
}

void PacketParser::encodeCoordinate(double value, uint8_t* out) {
    // Mirrors xhc-hb04.cc xhc_encode_float:
    //   int_v      = round(|v| * 10000)
    //   int_part   = int_v / 10000
    //   fract_part = int_v % 10000, bit 15 set when v < 0
    unsigned int int_v = static_cast<unsigned int>(
        llround(std::fabs(value) * 10000.0));
    uint16_t intPart = static_cast<uint16_t>(int_v / 10000u);
    uint16_t fractPart = static_cast<uint16_t>(int_v % 10000u);
    if (value < 0.0) fractPart = static_cast<uint16_t>(fractPart | 0x8000u);

    writeU16LE(out, intPart);
    writeU16LE(out + 2, fractPart);
}

void PacketParser::buildDisplayPayload(const DisplayData& d, uint8_t* out) {
    // Matches xhc-hb04.cc xhc_display_encode byte-for-byte.
    for (size_t i = 0; i < xhc::kDisplayBufSize; ++i) out[i] = 0;

    uint8_t* p = out;
    *p++ = 0xFE;
    *p++ = 0xFD;
    *p++ = 0x0C;

    // Work coordinates (line 1 = X or A when the A axis is active).
    encodeCoordinate(d.line1, p); p += 4;
    encodeCoordinate(d.line2, p); p += 4;
    encodeCoordinate(d.line3, p); p += 4;
    // Machine coordinates.
    encodeCoordinate(d.machine1, p); p += 4;
    encodeCoordinate(d.machine2, p); p += 4;
    encodeCoordinate(d.machine3, p); p += 4;

    // Override and rate values (x100 / x60 per xhc-hb04 man page).
    writeS16(p, static_cast<int>(llround(d.feedOverride * 100.0)));  p += 2;
    writeS16(p, static_cast<int>(llround(d.spindleOverride * 100.0))); p += 2;
    writeS16(p, static_cast<int>(llround(d.feedValue * 60.0)));      p += 2;
    writeS16(p, static_cast<int>(llround(d.spindleRps * 60.0)));     p += 2;

    out[xhc::kDispStepsizeByte] = stepsizeDisplayCode(d.stepsize);

    uint8_t flags = 0;
    if (d.inchIcon) flags |= xhc::kFlagInchIcon;
    out[xhc::kDispFlagsByte] = flags;
}

} // namespace mpgd::usb
