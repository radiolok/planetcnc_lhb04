#pragma once

// XHC LHB04 (wired pendant, VID 0x10CE / PID 0xEB70) USB HID protocol.
//
// The field layout and display encoding below are ported from the LinuxCNC
// "xhc-hb04" HAL component (src/hal/user_comps/xhc-hb04.cc), which is the
// reference implementation for the 10CE:EB70 device, and its button layout
// files (lib/hallib/xhc-hb04-layout{1,2}.cfg).
//
// Incoming report:   report ID 0x04 (HID interrupt IN endpoint). The reference
//                    XHC device sends 8 bytes; some clones (e.g. KTURT) send a
//                    6-byte report that omits the seed and checksum bytes.
// Outgoing display:  6 x 8-byte reports, each report ID 0x06 (SET_REPORT)

#include <cstdint>
#include <cstdio>
#include <string>

namespace mpgd::xhc {

// --- Report IDs -----------------------------------------------------------
inline constexpr uint8_t kInputReportId  = 0x04;
inline constexpr uint8_t kOutputReportId = 0x06;

// --- Packet sizes ---------------------------------------------------------
inline constexpr size_t kInputPacketSize  = 8;   // reference (XHC) report length
inline constexpr size_t kInputPacketSizeMin = 6; // KTURT clones send 6 bytes
inline constexpr size_t kDisplayReportSize = 8;   // one SET_REPORT transaction
inline constexpr size_t kDisplayReportsCount = 6; // reports per display frame
inline constexpr size_t kDisplayPayloadSize  = kDisplayReportSize * kDisplayReportsCount; // 48

// Display buffer: 6 x 7 payload bytes multiplexed across 6 reports, the first
// byte of each 8-byte report being the report ID (0x06).
inline constexpr size_t kDisplayBufSize = 42; // 6 * 7

// Offsets into the incoming 8-byte report (verified against xhc-hb04.cc).
inline constexpr size_t kOffsetReportId = 0;
inline constexpr size_t kOffsetButton1  = 1; // active button code (0 = none)
inline constexpr size_t kOffsetButton2  = 2; // secondary button code
inline constexpr size_t kOffsetAxis     = 3; // rotary selector position
inline constexpr size_t kOffsetJogDelta = 4; // MPG wheel delta, int8
inline constexpr size_t kOffsetFeed     = 5; // feed/stepsize rotary (reserved in reference driver)
inline constexpr size_t kOffsetSeed     = 6; // random seed byte
inline constexpr size_t kOffsetChecksum = 7; // XOR-based checksum

// --- Axis rotary selector (xhc-hb04.cc enum) ------------------------------
inline constexpr uint8_t kAxisOff     = 0x00;
inline constexpr uint8_t kAxisX       = 0x11;
inline constexpr uint8_t kAxisY       = 0x12;
inline constexpr uint8_t kAxisZ       = 0x13;
inline constexpr uint8_t kAxisSpindle = 0x14; // spindle override
inline constexpr uint8_t kAxisFeed    = 0x15; // feed override
inline constexpr uint8_t kAxisA       = 0x18;

// --- LCD stepsize display codes (STEPSIZE_BYTE) ---------------------------
inline constexpr uint8_t kStepDisplay0    = 0x00;
inline constexpr uint8_t kStepDisplay1    = 0x01;
inline constexpr uint8_t kStepDisplay5    = 0x02;
inline constexpr uint8_t kStepDisplay10   = 0x03;
inline constexpr uint8_t kStepDisplay20   = 0x04;
inline constexpr uint8_t kStepDisplay30   = 0x05;
inline constexpr uint8_t kStepDisplay40   = 0x06;
inline constexpr uint8_t kStepDisplay50   = 0x07;
inline constexpr uint8_t kStepDisplay100  = 0x08;
inline constexpr uint8_t kStepDisplay500  = 0x09;
inline constexpr uint8_t kStepDisplay1000 = 0x0A;

// --- Display buffer offsets (xhc-hb04.cc xhc_display_encode) ---------------
inline constexpr size_t kDispStepsizeByte = 35;
inline constexpr size_t kDispFlagsByte    = 36;

// FLAGS_BYTE bits
inline constexpr uint8_t kFlagInchIcon = 0x80;

// --- Button codes (18-button layout2.cfg; most common LHB04) ---------------
enum class Button : uint8_t {
    None        = 0x00,
    GotoZero    = 0x01,
    StartPause  = 0x02,
    Rewind      = 0x03,
    ProbeZ      = 0x04,
    Macro3      = 0x05,
    Half        = 0x06,
    Zero        = 0x07,
    SafeZ       = 0x08,
    Home        = 0x09,
    Macro1      = 0x0A,
    Macro2      = 0x0B,
    Spindle     = 0x0C,
    Step        = 0x0D,
    Mode        = 0x0E,
    Macro6      = 0x0F,
    Macro7      = 0x10,
    Stop        = 0x16,
    Reset       = 0x17,
};

// Canonical button names used as YAML mapping keys.
inline const char* buttonName(Button b) {
    switch (b) {
        case Button::GotoZero:   return "goto_zero";
        case Button::StartPause: return "start_pause";
        case Button::Rewind:     return "rewind";
        case Button::ProbeZ:     return "probe_z";
        case Button::Macro3:     return "macro_3";
        case Button::Half:       return "half";
        case Button::Zero:       return "zero";
        case Button::SafeZ:      return "safe_z";
        case Button::Home:       return "home";
        case Button::Macro1:     return "macro_1";
        case Button::Macro2:     return "macro_2";
        case Button::Spindle:    return "spindle";
        case Button::Step:       return "step";
        case Button::Mode:       return "mode";
        case Button::Macro6:     return "macro_6";
        case Button::Macro7:     return "macro_7";
        case Button::Stop:       return "stop";
        case Button::Reset:      return "reset";
        default:                 return "none";
    }
}

inline const char* buttonName(uint8_t code) {
    return buttonName(static_cast<Button>(code));
}

// Maps a raw button code to a canonical name; returns "none" for unknown codes.
inline std::string buttonNameOrHex(uint8_t code) {
    if (code == 0) return std::string("none");
    switch (static_cast<Button>(code)) {
        case Button::GotoZero: case Button::StartPause: case Button::Rewind:
        case Button::ProbeZ: case Button::Macro3: case Button::Half:
        case Button::Zero: case Button::SafeZ: case Button::Home:
        case Button::Macro1: case Button::Macro2: case Button::Spindle:
        case Button::Step: case Button::Mode: case Button::Macro6:
        case Button::Macro7: case Button::Stop: case Button::Reset:
            return std::string(buttonName(code));
        default: {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "0x%02X", code);
            return std::string(buf);
        }
    }
}

} // namespace mpgd::xhc
