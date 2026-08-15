#pragma once

#include "usb/XhcProtocol.h"

#include <cstdint>
#include <string>

namespace mpgd::usb {

// One parsed incoming report (device -> PC).
struct ParsedInput {
    uint8_t reportId = 0;
    uint8_t button1 = 0;   // active button code (0 = none)
    uint8_t button2 = 0;   // secondary button code
    uint8_t axisCode = 0;  // raw rotary selector position
    int8_t  jogDelta = 0;  // MPG wheel delta (-128..127)
    uint8_t feedRotary = 0;
    uint8_t seed = 0;
    uint8_t checksumByte = 0;
    uint8_t expectedChecksum = 0;
    bool checksumOk = true;
    // True when the report carries a checksum byte (8-byte reference reports).
    // 6-byte KTURT reports have no seed/checksum, so checksumOk stays true and
    // this flag lets callers know the check was not performed.
    bool hasChecksum = false;
};

// Everything required to render one LCD frame (wire format, not screen units).
struct DisplayData {
    // Coordinates in mm. line1/line2/line3 are the three LCD lines (work
    // coordinates). machine1/2/3 are the three machine-coordinate lines.
    double line1 = 0.0, line2 = 0.0, line3 = 0.0;
    double machine1 = 0.0, machine2 = 0.0, machine3 = 0.0;
    double feedOverride = 0.0;    // fraction 0..1 (x100 for display)
    double spindleOverride = 0.0; // fraction 0..1 (x100 for display)
    double feedValue = 0.0;       // current feed in units/sec (x60 for display)
    double spindleRps = 0.0;      // spindle speed in RPS (x60 for display)
    int stepsize = 1;             // 1/10/100/1000 (stepsize sequence value)
    bool inchIcon = false;
    // When true, line1/machine1 are interpreted as the A axis (like xhc-hb04).
    bool aAxisActive = false;
};

// Pure, allocation-free parsing/encoding of the LHB04 wire protocol. Kept
// separate from the HID transport so the checksum and field offsets can be
// unit-tested against captured packets.
class PacketParser {
public:
    // Parses an input report (report ID 0x04). Accepts reports of at least
    // kInputPacketSizeMin (6) bytes; the seed/checksum fields are only filled
    // in for full 8-byte reports. Returns a default-initialized struct with
    // reportId != kInputReportId if `len` is too short or the report ID does
    // not match.
    static ParsedInput parseInput(const uint8_t* data, size_t len);

    // XOR checksum over bytes [1..6] (reference implementations use XOR).
    static uint8_t computeChecksum(const uint8_t* data, size_t len);

    // Encodes a coordinate into 4 bytes: int16 magnitude + int16 fraction
    // (fraction x 10000, bit 15 = sign). Matches xhc-hb04.cc xhc_encode_float.
    static void encodeCoordinate(double value, uint8_t* out);

    // Builds the 42-byte display payload buffer (see xhc-hb04.cc
    // xhc_display_encode) into `out`, which must hold kDisplayBufSize bytes.
    static void buildDisplayPayload(const DisplayData& d, uint8_t* out);

    // Maps a raw axis rotary code to a step size selection (in the configured
    // sequence) for the feed knob. Reserved: the reference driver controls the
    // step size in software rather than reading it from the device.
};

} // namespace mpgd::usb
