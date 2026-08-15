#include "test_framework.h"
#include "usb/PacketParser.h"

#include <cstring>

using namespace mpgd;
using namespace mpgd::usb;

static void test_parse_input_fields() {
    // Simulate an 8-byte report: report 0x04, button 0x09 (home), axis 0x11
    // (X), jog delta +3, seed 0x42.
    uint8_t pkt[8] = {0x04, 0x09, 0x00, 0x11, 0x03, 0x00, 0x42, 0x00};
    pkt[7] = PacketParser::computeChecksum(pkt, 8);

    ParsedInput p = PacketParser::parseInput(pkt, 8);
    CHECK_EQ(p.reportId, xhc::kInputReportId);
    CHECK_EQ(p.button1, 0x09);
    CHECK_EQ(p.axisCode, xhc::kAxisX);
    CHECK_EQ(static_cast<int>(p.jogDelta), 3);
    CHECK(p.hasChecksum);
    CHECK(p.checksumOk);
}

static void test_parse_six_byte_report() {
    // KTURT variant: 6-byte report without seed/checksum.
    uint8_t pkt[6] = {0x04, 0x09, 0x00, 0x11, 0x03, 0x09};
    ParsedInput p = PacketParser::parseInput(pkt, 6);
    CHECK_EQ(p.reportId, xhc::kInputReportId);
    CHECK_EQ(p.button1, 0x09);
    CHECK_EQ(p.axisCode, xhc::kAxisX);
    CHECK_EQ(static_cast<int>(p.jogDelta), 3);
    CHECK(!p.hasChecksum);
    CHECK(p.checksumOk); // no checksum byte -> treated as ok
}

static void test_parse_wrong_report_id() {
    uint8_t pkt[8] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ParsedInput p = PacketParser::parseInput(pkt, 8);
    CHECK(p.reportId != xhc::kInputReportId);
}

static void test_parse_short_buffer() {
    uint8_t pkt[5] = {0x04, 0x00, 0x00, 0x00, 0x00};
    ParsedInput p = PacketParser::parseInput(pkt, 5);
    CHECK(p.reportId != xhc::kInputReportId);
}

static void test_negative_jog_delta() {
    uint8_t pkt[8] = {0x04, 0x00, 0x00, 0x12, 0xFE, 0x00, 0x00, 0x00}; // -2
    pkt[7] = PacketParser::computeChecksum(pkt, 8);
    ParsedInput p = PacketParser::parseInput(pkt, 8);
    CHECK_EQ(static_cast<int>(p.jogDelta), -2);
}

static void test_checksum_vector() {
    // XOR of bytes [1..6].
    uint8_t pkt[8] = {0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00};
    // 0x01 ^ 0x02 = 0x03; ^0x03 = 0x00; ^0x04 = 0x04; ^0x05 = 0x01; ^0x06 = 0x07
    CHECK_EQ(PacketParser::computeChecksum(pkt, 8), 0x07);
}

static void test_checksum_mismatch_detection() {
    uint8_t pkt[8] = {0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xAA};
    ParsedInput p = PacketParser::parseInput(pkt, 8);
    CHECK(!p.checksumOk);
    CHECK_EQ(p.expectedChecksum, 0x07);
    CHECK_EQ(p.checksumByte, 0xAA);
}

int main() {
    test_parse_input_fields();
    test_parse_six_byte_report();
    test_parse_wrong_report_id();
    test_parse_short_buffer();
    test_negative_jog_delta();
    test_checksum_vector();
    test_checksum_mismatch_detection();
    return tfw::summary("test_packet_parser");
}
