// MPG adapter test GUI for the XHC LHB04 pendant.
//
// Opens the HID device directly (reusing the daemon's HidDevice + PacketParser
// from mpgd_core) and renders the live, raw state so you can verify the
// adapter by turning the MPG wheel / rotary selectors and pressing buttons.

#include "usb/HidDevice.h"
#include "usb/PacketParser.h"
#include "usb/XhcProtocol.h"

#include <hidapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t kVendorId = 0x10CE;
const std::vector<uint16_t> kProductIds{0xEB70, 0xEB71, 0xEB93};

// Latest pendant state, written by the reader thread, read by the UI thread.
struct Snapshot {
    bool connected = false;
    bool havePacket = false;
    std::string manufacturer;
    std::string product;
    uint8_t raw[8] = {0};
    uint8_t rawLen = 0;
    uint8_t reportId = 0;
    uint8_t button1 = 0;
    uint8_t button2 = 0;
    uint8_t axisCode = 0;
    int8_t jogDelta = 0;
    uint8_t feedRotary = 0;
    uint8_t seed = 0;
    uint8_t checksumByte = 0;
    uint8_t expectedChecksum = 0;
    bool checksumOk = true;
    bool sleeping = false;
    int64_t jogAccum = 0;
    int64_t axisAccum[6] = {0}; // X, Y, Z, A, Spindle, Feed
    uint64_t packetCount = 0;
    uint64_t checksumFails = 0;
};

struct LogEntry {
    std::string stamp;
    std::string text;
};

std::mutex g_mtx;
Snapshot g_snap;
std::deque<LogEntry> g_log;
std::atomic<bool> g_shutdown{false};
ImFont* g_mono = nullptr;

// Every raw packet is appended here (full fidelity, unthrottled) so the
// capture can be shared/analyzed even though the in-memory log is throttled.
const std::string g_rawLogPath = "mpg_gui_raw.log";
std::ofstream g_rawFile;

std::string nowStamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

void logAdd(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_log.push_back({nowStamp(), text});
    while (g_log.size() > 1000) g_log.pop_front();
}

const char* axisName(uint8_t code) {
    switch (code) {
        case mpgd::xhc::kAxisOff:     return "OFF";
        case mpgd::xhc::kAxisX:       return "X";
        case mpgd::xhc::kAxisY:       return "Y";
        case mpgd::xhc::kAxisZ:       return "Z";
        case mpgd::xhc::kAxisA:       return "A";
        case mpgd::xhc::kAxisSpindle: return "Spindle";
        case mpgd::xhc::kAxisFeed:    return "Feed";
        default:                      return "?";
    }
}

// Maps the axis rotary position to the LCD variable the wheel drives:
// X, Y, Z, A, Spindle, Feed (indexes 0..5). Returns -1 for OFF/unknown.
int axisVariableIndex(uint8_t code) {
    switch (code) {
        case mpgd::xhc::kAxisX:       return 0;
        case mpgd::xhc::kAxisY:       return 1;
        case mpgd::xhc::kAxisZ:       return 2;
        case mpgd::xhc::kAxisA:       return 3;
        case mpgd::xhc::kAxisSpindle: return 4;
        case mpgd::xhc::kAxisFeed:    return 5;
        default:                      return -1;
    }
}

const char* axisVariableName(int idx) {
    static const char* names[] = {"X", "Y", "Z", "A", "S", "F"};
    return (idx >= 0 && idx < 6) ? names[idx] : "?";
}

// Renders the six LCD variables into the 6 x 8-byte output reports that the
// pendant displays (report ID 0x06, same wire layout as the daemon).
//
// The mapping follows the reference driver: the three work lines show
// X/Y/Z (line 1 shows A when the A axis is selected), the three machine
// lines mirror X/Y/Z, and Spindle/Feed are exposed as override percentages.
void buildDisplayFrame(const int64_t (&acc)[6], uint8_t axisCode,
                       uint8_t (&reports)[mpgd::xhc::kDisplayReportsCount]
                                          [mpgd::xhc::kDisplayReportSize]) {
    const bool aActive = (axisCode == mpgd::xhc::kAxisA);

    mpgd::usb::DisplayData d;
    d.line1 = static_cast<double>(aActive ? acc[3] : acc[0]);
    d.line2 = static_cast<double>(acc[1]);
    d.line3 = static_cast<double>(acc[2]);
    d.machine1 = static_cast<double>(aActive ? acc[3] : acc[0]);
    d.machine2 = static_cast<double>(acc[1]);
    d.machine3 = static_cast<double>(acc[2]);
    d.feedOverride = static_cast<double>(acc[5]) / 100.0;
    d.spindleOverride = static_cast<double>(acc[4]) / 100.0;
    d.feedValue = 0.0;
    d.spindleRps = 0.0;
    d.stepsize = 1;
    d.inchIcon = false;
    d.aAxisActive = aActive;

    uint8_t payload[mpgd::xhc::kDisplayBufSize];
    mpgd::usb::PacketParser::buildDisplayPayload(d, payload);
    for (size_t r = 0; r < mpgd::xhc::kDisplayReportsCount; ++r) {
        reports[r][0] = mpgd::xhc::kOutputReportId;
        for (size_t i = 0; i < 7; ++i)
            reports[r][i + 1] = payload[r * 7 + i];
    }
}

std::string hexString(const uint8_t* data, size_t len) {
    std::string out;
    char b[4];
    for (size_t i = 0; i < len; ++i) {
        std::snprintf(b, sizeof(b), "%02X ", data[i]);
        out += b;
    }
    return out;
}

// Best-effort parse of a report shorter than the 8 bytes the reference driver
// expects. Assumes the same field order (report id, button1, button2, axis,
// jog delta, feed, seed, checksum) and just stops at the available bytes.
mpgd::usb::ParsedInput parsePartial(const uint8_t* buf, size_t len) {
    mpgd::usb::ParsedInput p;
    if (len < 1) return p;
    p.reportId = buf[0];
    if (len >= 2) p.button1 = buf[1];
    if (len >= 3) p.button2 = buf[2];
    if (len >= 4) p.axisCode = buf[3];
    if (len >= 5) p.jogDelta = static_cast<int8_t>(buf[4]);
    if (len >= 6) p.feedRotary = buf[5];
    if (len >= 7) p.seed = buf[6];
    return p;
}

// Opens the pendant for both input reads and display writes. On Windows the
// pendant enumerates as several HID top-level collections: one exposes the
// input report (0x04) and another the feature report (0x06) used for the LCD.
// We probe each node with a feature-report write to find the write-capable one.
bool openPendant(mpgd::usb::HidDevice& readDev, mpgd::usb::HidDevice& writeDev,
                 std::string& err) {
    const auto devs = mpgd::usb::enumerateDevices(kVendorId, kProductIds);
    if (devs.empty()) {
        err = "no XHC LHB04 device found";
        return false;
    }

    if (devs.size() == 1) {
        if (!readDev.openPath(devs[0].path) || !writeDev.openPath(devs[0].path)) {
            err = "open failed";
            return false;
        }
        return true;
    }

    mpgd::usb::HidDeviceInfo readInfo;
    mpgd::usb::HidDeviceInfo writeInfo;
    bool haveRead = false;
    bool haveWrite = false;
    for (const auto& info : devs) {
        mpgd::usb::HidDevice probe;
        if (!probe.openPath(info.path)) continue;
        uint8_t rep[mpgd::xhc::kDisplayReportSize] = {
            mpgd::xhc::kOutputReportId, 0, 0, 0, 0, 0, 0, 0};
        const int wr = probe.sendFeatureReport(rep, sizeof(rep));
        if (wr >= 0) {
            if (!haveWrite) { writeInfo = info; haveWrite = true; }
        } else {
            if (!haveRead) { readInfo = info; haveRead = true; }
        }
    }

    if (haveWrite && haveRead) {
        if (!readDev.openPath(readInfo.path) || !writeDev.openPath(writeInfo.path)) {
            err = "open read/write collections failed";
            return false;
        }
        return true;
    }

    // Could not separate roles; use the first node for both.
    if (!readDev.openPath(devs[0].path) || !writeDev.openPath(devs[0].path)) {
        err = "open failed";
        return false;
    }
    return true;
}

void readerThread() {
    mpgd::usb::HidDevice readDev;
    mpgd::usb::HidDevice writeDev;
    const int reconnectMs = 2000;
    uint8_t lastButton = 0;
    bool wasConnected = false;
    auto lastBtnEdge = std::chrono::steady_clock::now();
    auto lastWheelLog = std::chrono::steady_clock::now();
    std::string lastRawHex;
    auto lastRawLog = std::chrono::steady_clock::now();
    auto lastNoDataLog = std::chrono::steady_clock::now();
    auto lastDataTp = std::chrono::steady_clock::now();
    uint8_t lastBadReportId = 0xFF;
    auto lastDisplayTp = std::chrono::steady_clock::now();
    bool lastDisplayWriteFailed = false;
    bool displayModeLogged = false;

    // Capture every raw packet to a file so it can be shared/analyzed.
    g_rawFile.open(g_rawLogPath, std::ios::app);

    while (!g_shutdown.load()) {
        if (!readDev.isOpen() || !writeDev.isOpen()) {
            std::string err;
            if (openPendant(readDev, writeDev, err)) {
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_snap.connected = true;
                    g_snap.manufacturer = readDev.manufacturer();
                    g_snap.product = readDev.product();
                }
                logAdd("Connected: " + readDev.manufacturer() + " " + readDev.product());
                logAdd("display via feature report (0x06)");
                wasConnected = true;
            } else {
                if (wasConnected) {
                    logAdd("Disconnected: " + err);
                    wasConnected = false;
                }
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_snap.connected = false;
                }
                for (int i = 0; i < reconnectMs / 50 && !g_shutdown.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
        }

        // Read into a larger buffer so the true report length is visible even
        // if it differs from the 8 bytes the reference driver expects.
        uint8_t buf[64] = {0};
        int r = readDev.read(buf, sizeof(buf), 50);

        if (r > 0) {
            lastDataTp = std::chrono::steady_clock::now();
            const std::string hex = hexString(buf, static_cast<size_t>(r));

            if (g_rawFile.is_open()) {
                g_rawFile << nowStamp() << " len=" << r << "  " << hex << "\n";
                g_rawFile.flush();
            }

            // Throttled in-memory copy: log when the bytes change or at most
            // ~2x/second for a steady stream.
            auto now = std::chrono::steady_clock::now();
            auto sinceLog =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRawLog).count();
            if (hex != lastRawHex || sinceLog >= 500) {
                char msg[96];
                std::snprintf(msg, sizeof(msg), "raw[%d]: %s", r, hex.c_str());
                logAdd(msg);
                lastRawLog = now;
                lastRawHex = hex;
            }

            // Parse what we got. r > 0 is data, never a fatal error: a report
            // shorter/longer than 8 bytes is handled best-effort instead of
            // dropping the connection.
            mpgd::usb::ParsedInput p =
                (r >= 8) ? mpgd::usb::PacketParser::parseInput(buf, 8)
                         : parsePartial(buf, static_cast<size_t>(r));

            if (p.reportId != mpgd::xhc::kInputReportId) {
                if (p.reportId != lastBadReportId) {
                    char msg[64];
                    std::snprintf(msg, sizeof(msg), "unexpected report id 0x%02X (len=%d)",
                                  p.reportId, r);
                    logAdd(msg);
                    lastBadReportId = p.reportId;
                }
            }

            const bool sleeping = (p.button1 == 0 && p.button2 == 0 &&
                                   p.axisCode == 0 && p.jogDelta == 0);

            std::string eventText;
            bool hasEvent = false;
            if (p.button1 != lastButton) {
                auto now2 = std::chrono::steady_clock::now();
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now2 - lastBtnEdge).count();
                if (elapsed >= 15) {
                    if (p.button1 != 0)
                        eventText = "Button press: " + mpgd::xhc::buttonNameOrHex(p.button1);
                    else
                        eventText = "Button release: " + mpgd::xhc::buttonNameOrHex(lastButton);
                    hasEvent = true;
                    lastBtnEdge = now2;
                }
                lastButton = p.button1;
            }

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                Snapshot& s = g_snap;
                s.havePacket = true;
                std::memset(s.raw, 0, sizeof(s.raw));
                const size_t n = static_cast<size_t>(r) < sizeof(s.raw)
                                     ? static_cast<size_t>(r)
                                     : sizeof(s.raw);
                std::memcpy(s.raw, buf, n);
                s.rawLen = static_cast<uint8_t>(n);
                s.reportId = p.reportId;
                s.button1 = p.button1;
                s.button2 = p.button2;
                s.axisCode = p.axisCode;
                s.jogDelta = p.jogDelta;
                s.feedRotary = p.feedRotary;
                s.seed = p.seed;
                s.checksumByte = p.checksumByte;
                s.expectedChecksum = p.expectedChecksum;
                s.checksumOk = p.checksumOk;
                s.sleeping = sleeping;
                if (p.jogDelta != 0) {
                    s.jogAccum += static_cast<int>(p.jogDelta);
                    const int vi = axisVariableIndex(p.axisCode);
                    if (vi >= 0) s.axisAccum[vi] += static_cast<int>(p.jogDelta);
                }
                s.packetCount++;
                if (!p.checksumOk) s.checksumFails++;
            }

            if (hasEvent) {
                logAdd(eventText);
            } else if (p.jogDelta != 0) {
                auto now2 = std::chrono::steady_clock::now();
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now2 - lastWheelLog).count();
                if (elapsed >= 100) {
                    char b[32];
                    std::snprintf(b, sizeof(b), "Wheel: %+d", static_cast<int>(p.jogDelta));
                    logAdd(b);
                    lastWheelLog = now2;
                }
            }
        } else if (r == 0) {
            // Connected but no report arrived — surface it about once a second
            // so a silently-idle device is visible in the log.
            auto now = std::chrono::steady_clock::now();
            auto sinceData =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDataTp).count();
            auto sinceLog =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastNoDataLog).count();
            if (sinceData >= 1000 && sinceLog >= 1000) {
                logAdd("no input reports (read timeout)");
                lastNoDataLog = now;
            }
        } else { // r < 0: real hidapi error
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Read error (%d), reconnecting...", r);
            readDev.close();
            writeDev.close();
            logAdd(msg);
            wasConnected = false;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_snap.connected = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnectMs));
        }

        // Feedback path (PC -> pendant): refresh the LCD with the current
        // XYZASF accumulators at ~25 Hz max so the display is no longer blank
        // and the wheel visibly changes only the selected variable.
        if (writeDev.isOpen()) {
            const auto nowD = std::chrono::steady_clock::now();
            const auto sinceDisplay =
                std::chrono::duration_cast<std::chrono::milliseconds>(nowD - lastDisplayTp).count();
            if (sinceDisplay >= 40) {
                int64_t acc[6];
                uint8_t axisCode = 0;
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    for (int i = 0; i < 6; ++i) acc[i] = g_snap.axisAccum[i];
                    axisCode = g_snap.axisCode;
                }
                uint8_t frame[mpgd::xhc::kDisplayReportsCount]
                             [mpgd::xhc::kDisplayReportSize];
                buildDisplayFrame(acc, axisCode, frame);

                int firstWr = 0;
                for (size_t rr = 0; rr < mpgd::xhc::kDisplayReportsCount; ++rr) {
                    const int wr = writeDev.sendFeatureReport(frame[rr], mpgd::xhc::kDisplayReportSize);
                    if (rr == 0) firstWr = wr;
                    if (wr < 0) {
                        if (!lastDisplayWriteFailed) {
                            logAdd("display write failed (" + writeDev.lastError() + ")");
                            lastDisplayWriteFailed = true;
                        }
                        break;
                    }
                    lastDisplayWriteFailed = false;
                }
                if (!displayModeLogged && !lastDisplayWriteFailed) {
                    displayModeLogged = true;
                    logAdd("display feedback active (feature report, write #0 -> " +
                           std::to_string(firstWr) + ")");
                }
                lastDisplayTp = nowD;
            }
        }
    }

    if (g_rawFile.is_open())
        g_rawFile.close();
    readDev.close();
    writeDev.close();
}

void DrawUI() {
    Snapshot s;
    std::vector<LogEntry> logs;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        s = g_snap;
        logs.assign(g_log.begin(), g_log.end());
    }

    const std::string b1 = s.button1 ? mpgd::xhc::buttonNameOrHex(s.button1) : std::string("none");
    const std::string b2 = s.button2 ? mpgd::xhc::buttonNameOrHex(s.button2) : std::string("none");

    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 700), ImGuiCond_FirstUseEver);
    ImGui::Begin("MPG adapter test");

    if (s.connected) {
        ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.35f, 1.0f), "CONNECTED");
        ImGui::SameLine();
        ImGui::Text("%s %s", s.manufacturer.c_str(), s.product.c_str());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "DISCONNECTED - waiting for XHC LHB04...");
    }
    ImGui::TextDisabled("VID 0x%04X  PIDs 0xEB70/0xEB71/0xEB93", kVendorId);

    ImGui::Separator();
    ImGui::Text("Raw report (%d bytes):", s.rawLen);
    char raw[64];
    int o = 0;
    if (s.havePacket) {
        for (int i = 0; i < s.rawLen && i < 8; ++i)
            o += std::snprintf(raw + o, sizeof(raw) - o, "%02X ", s.raw[i]);
    } else {
        o += std::snprintf(raw + o, sizeof(raw) - o, "-- -- -- -- -- -- -- --");
    }
    if (g_mono) ImGui::PushFont(g_mono);
    ImGui::TextUnformatted(raw);
    if (g_mono) ImGui::PopFont();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy raw"))
        ImGui::SetClipboardText(raw);

    ImGui::Separator();
    if (ImGui::BeginTable("fields", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Value");

        auto row = [&](const char* label, const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            char buf[256];
            std::vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(buf);
        };

        row("Report ID", s.havePacket ? "0x%02X" : "-", s.reportId);
        row("Button 1", "%s (0x%02X)", b1.c_str(), s.button1);
        row("Button 2", "%s (0x%02X)", b2.c_str(), s.button2);
        row("Axis selector", "%s (0x%02X)", axisName(s.axisCode), s.axisCode);
        row("Jog delta", s.havePacket ? "%+d" : "-", static_cast<int>(s.jogDelta));
        row("Feed rotary", s.havePacket ? "0x%02X" : "-", s.feedRotary);
        row("Seed", s.havePacket ? "0x%02X" : "-", s.seed);

        char cs[64];
        if (s.havePacket) {
            std::snprintf(cs, sizeof(cs), "rx 0x%02X / calc 0x%02X", s.checksumByte,
                          s.expectedChecksum);
        } else {
            std::snprintf(cs, sizeof(cs), "-");
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Checksum");
        ImGui::TableSetColumnIndex(1);
        if (s.havePacket) {
            ImGui::TextColored(s.checksumOk ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f)
                                            : ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                               "%s  %s", cs, s.checksumOk ? "OK" : "MISMATCH");
        } else {
            ImGui::TextUnformatted("-");
        }

        row("Sleeping", "%s", s.sleeping ? "yes" : "no");
        row("Packets", "%llu", static_cast<unsigned long long>(s.packetCount));
        row("Checksum fails", "%llu", static_cast<unsigned long long>(s.checksumFails));
        row("Wheel accum.", "%lld", static_cast<long long>(s.jogAccum));

        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("Axis selector:");
    const struct { uint8_t code; const char* name; } axes[] = {
        {mpgd::xhc::kAxisOff, "OFF"}, {mpgd::xhc::kAxisX, "X"},
        {mpgd::xhc::kAxisY, "Y"},     {mpgd::xhc::kAxisZ, "Z"},
        {mpgd::xhc::kAxisA, "A"},     {mpgd::xhc::kAxisSpindle, "Spindle"},
        {mpgd::xhc::kAxisFeed, "Feed"},
    };
    for (const auto& a : axes) {
        if (s.axisCode == a.code)
            ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.4f, 1.0f), "[%s]", a.name);
        else
            ImGui::TextDisabled("%s", a.name);
        ImGui::SameLine(0.0f, 12.0f);
    }
    ImGui::NewLine();

    ImGui::Separator();
    ImGui::Text("MPG wheel:");
    const char* dir = s.jogDelta == 0 ? "-"
                      : (s.jogDelta > 0 ? "clockwise (+)" : "counter-clockwise (-)");
    ImGui::Text("Accumulated: %lld   Last: %+d  (%s)", static_cast<long long>(s.jogAccum),
                static_cast<int>(s.jogDelta), dir);

    ImGui::Separator();
    ImGui::Text("Pendant display feedback (XYZASF):");
    const int vi = axisVariableIndex(s.axisCode);
    for (int i = 0; i < 6; ++i) {
        if (i == vi)
            ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.4f, 1.0f), "%s=%lld",
                               axisVariableName(i), static_cast<long long>(s.axisAccum[i]));
        else
            ImGui::TextDisabled("%s=%lld", axisVariableName(i),
                                static_cast<long long>(s.axisAccum[i]));
        ImGui::SameLine(0.0f, 18.0f);
    }
    ImGui::NewLine();
    ImGui::TextDisabled("wheel updates only the selected variable; X/Y/Z on the "
                        "3 LCD lines, A on line 1, S/F as override %%");

    ImGui::Separator();
    ImGui::Text("Button:");
    if (s.button1)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s (0x%02X)", b1.c_str(),
                           s.button1);
    else
        ImGui::TextDisabled("none");

    ImGui::Separator();
    if (ImGui::Button("Reset counters")) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_snap.jogAccum = 0;
        for (int i = 0; i < 6; ++i) g_snap.axisAccum[i] = 0;
        g_snap.packetCount = 0;
        g_snap.checksumFails = 0;
        g_snap.havePacket = false;
    }

    ImGui::End();

    // --- event log ----------------------------------------------------------
    ImGui::SetNextWindowPos(ImVec2(585, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 700), ImGuiCond_FirstUseEver);
    ImGui::Begin("Event log");
    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_log.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy log")) {
        std::string all;
        for (const auto& e : logs)
            all += e.stamp + "  " + e.text + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Text("%zu events", logs.size());
    ImGui::TextDisabled("full capture: %s", g_rawLogPath.c_str());
    ImGui::Separator();

    ImGui::BeginChild("scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    if (g_mono) ImGui::PushFont(g_mono);
    for (const auto& e : logs) {
        ImGui::TextUnformatted(e.stamp.c_str());
        ImGui::SameLine(95.0f);
        ImGui::TextUnformatted(e.text.c_str());
    }
    if (g_mono) ImGui::PopFont();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)
                return 0; // disable the ALT menu
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

extern "C" int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"mpg_gui_class";
    if (!RegisterClassExW(&wc))
        return 1;

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"MPG adapter test",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1200, 800, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
        return 1;

    HDC hDc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int pf = ChoosePixelFormat(hDc, &pfd);
    SetPixelFormat(hDc, pf, &pfd);
    HGLRC hRC = wglCreateContext(hDc);
    wglMakeCurrent(hDc, hRC);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    g_mono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f);

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplOpenGL3_Init("#version 130");

    if (hid_init() != 0)
        logAdd("hid_init failed");

    std::thread reader(readerThread);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    constexpr std::chrono::microseconds kFrame{16667}; // ~60 FPS cap
    while (!g_shutdown.load()) {
        const auto frameStart = std::chrono::steady_clock::now();

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                g_shutdown.store(true);
        }
        if (g_shutdown.load())
            break;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUI();

        ImGui::Render();
        RECT rc;
        GetClientRect(hwnd, &rc);
        glViewport(0, 0, static_cast<int>(rc.right - rc.left),
                   static_cast<int>(rc.bottom - rc.top));
        glClearColor(0.09f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(hDc);

        const auto frameEnd = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart);
        if (elapsed < kFrame)
            std::this_thread::sleep_for(kFrame - elapsed);
    }

    g_shutdown.store(true);
    if (reader.joinable())
        reader.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hRC);
    ReleaseDC(hwnd, hDc);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    hid_exit();
    return 0;
}
