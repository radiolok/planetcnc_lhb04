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

void readerThread() {
    mpgd::usb::HidDevice dev;
    const int reconnectMs = 2000;
    uint8_t lastButton = 0;
    bool wasConnected = false;
    auto lastBtnEdge = std::chrono::steady_clock::now();
    auto lastWheelLog = std::chrono::steady_clock::now();

    while (!g_shutdown.load()) {
        if (!dev.isOpen()) {
            std::string err;
            if (dev.open(kVendorId, kProductIds, err)) {
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_snap.connected = true;
                    g_snap.manufacturer = dev.manufacturer();
                    g_snap.product = dev.product();
                }
                logAdd("Connected: " + dev.manufacturer() + " " + dev.product());
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

        uint8_t buf[mpgd::xhc::kInputPacketSize] = {0};
        int r = dev.read(buf, sizeof(buf), 50);

        if (r == static_cast<int>(mpgd::xhc::kInputPacketSize)) {
            mpgd::usb::ParsedInput p = mpgd::usb::PacketParser::parseInput(buf, sizeof(buf));
            if (p.reportId != mpgd::xhc::kInputReportId)
                continue;

            const bool sleeping = (p.button1 == 0 && p.button2 == 0 &&
                                   p.axisCode == 0 && p.jogDelta == 0);

            std::string eventText;
            bool hasEvent = false;
            if (p.button1 != lastButton) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBtnEdge).count();
                if (elapsed >= 15) {
                    if (p.button1 != 0)
                        eventText = "Button press: " + mpgd::xhc::buttonNameOrHex(p.button1);
                    else
                        eventText = "Button release: " + mpgd::xhc::buttonNameOrHex(lastButton);
                    hasEvent = true;
                    lastBtnEdge = now;
                }
                lastButton = p.button1;
            }

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                Snapshot& s = g_snap;
                s.havePacket = true;
                std::memcpy(s.raw, buf, sizeof(buf));
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
                if (p.jogDelta != 0) s.jogAccum += static_cast<int>(p.jogDelta);
                s.packetCount++;
                if (!p.checksumOk) s.checksumFails++;
            }

            if (hasEvent) {
                logAdd(eventText);
            } else if (p.jogDelta != 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastWheelLog).count();
                if (elapsed >= 100) {
                    char b[32];
                    std::snprintf(b, sizeof(b), "Wheel: %+d", static_cast<int>(p.jogDelta));
                    logAdd(b);
                    lastWheelLog = now;
                }
            }
        } else if (r < 0) {
            dev.close();
            logAdd("Read error, reconnecting...");
            wasConnected = false;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_snap.connected = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnectMs));
        }
        // r == 0: read timeout, no data — loop again.
    }

    dev.close();
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
    ImGui::Text("Raw report:");
    char raw[64];
    int o = 0;
    if (s.havePacket) {
        for (int i = 0; i < 8; ++i)
            o += std::snprintf(raw + o, sizeof(raw) - o, "%02X ", s.raw[i]);
    } else {
        o += std::snprintf(raw + o, sizeof(raw) - o, "-- -- -- -- -- -- -- --");
    }
    if (g_mono) ImGui::PushFont(g_mono);
    ImGui::TextUnformatted(raw);
    if (g_mono) ImGui::PopFont();

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
    ImGui::Text("%zu events", logs.size());
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
