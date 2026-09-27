#include "config/ConfigManager.h"
#include "logic/ButtonHandler.h"
#include "logic/ButtonQueue.h"
#include "logic/DisplayUpdater.h"
#include "logic/JogController.h"
#include "logic/SharedState.h"
#include "planetcnc/StateReader.h"
#include "planetcnc/TngApi.h"
#include "threads/ButtonThread.h"
#include "threads/DisplayThread.h"
#include "threads/JogThread.h"
#include "threads/UsbPollThread.h"
#include "usb/HidDevice.h"
#include "usb/XhcPendant.h"
#include "utils/Daemon.h"
#include "utils/Logger.h"

#include <hidapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CliOptions {
    std::string configPath = "mpgd.yaml";
    std::string profile;
    bool noGui = false;
    bool attach = false;
    bool sniff = false;
    bool list = false;
    bool help = false;
};

void printUsage(const char* argv0) {
    std::printf(
        "mpgd - XHC LHB04 pendant <-> PlanetCNC TNG adapter\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --config <path>   Path to the YAML config file (default: mpgd.yaml)\n"
        "  --profile <name>  PlanetCNC profile name to load\n"
        "  --no-gui          Run TNG headless (Run(true)); default runs with GUI\n"
        "  --attach          Attach to an already-running external TNG process\n"
        "  --sniff           Capture/log raw pendant packets only (no TNG)\n"
        "  --list            List HID devices and exit\n"
        "  --help            Show this help\n",
        argv0);
}

bool parseArgs(int argc, char** argv, CliOptions& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--config") {
            const char* v = need("--config");
            if (!v) return false;
            opts.configPath = v;
        } else if (arg == "--profile") {
            const char* v = need("--profile");
            if (!v) return false;
            opts.profile = v;
        } else if (arg == "--no-gui") {
            opts.noGui = true;
        } else if (arg == "--attach") {
            opts.attach = true;
        } else if (arg == "--sniff") {
            opts.sniff = true;
        } else if (arg == "--list") {
            opts.list = true;
        } else if (arg == "--help" || arg == "-h") {
            opts.help = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            return false;
        }
    }
    return true;
}

void listHidDevices() {
    if (hid_init() != 0) {
        std::fprintf(stderr, "hid_init failed\n");
        return;
    }
    hid_device_info* devs = hid_enumerate(0x10CE, 0x0);
    for (hid_device_info* d = devs; d; d = d->next) {
        std::printf("VID 0x%04X PID 0x%04X  %ls  %ls  (%s)\n",
                    d->vendor_id, d->product_id,
                    d->manufacturer_string ? d->manufacturer_string : L"?",
                    d->product_string ? d->product_string : L"?",
                    d->path ? d->path : "");
    }
    hid_free_enumeration(devs);
    hid_exit();
}

} // namespace

int main(int argc, char** argv) {
    CliOptions opts;
    if (!parseArgs(argc, argv, opts)) {
        printUsage(argv[0]);
        return 2;
    }
    if (opts.help) {
        printUsage(argv[0]);
        return 0;
    }
    if (opts.list) {
        listHidDevices();
        return 0;
    }

    // Load config (defaults overlaid with the file). A broken config is
    // fatal: falling back to defaults would silently change button bindings
    // and jog settings.
    mpgd::Config cfg;
    std::string cfgError;
    bool cfgMissing = false;
    if (!mpgd::ConfigManager::loadOrDefault(opts.configPath, cfg, cfgError,
                                            cfgMissing)) {
        std::fprintf(stderr, "error: invalid config %s:\n%s\n",
                     opts.configPath.c_str(), cfgError.c_str());
        return 2;
    }
    std::string logError;
    if (!mpgd::Logger::init(cfg.logging.level, cfg.logging.file, logError)) {
        mpgd::logError("logging: %s", logError.c_str());
    }
    if (cfgMissing) {
        mpgd::logWarn("config: %s not found; using built-in defaults",
                      opts.configPath.c_str());
    }

    if (!opts.profile.empty()) cfg.planetcnc.profile = opts.profile;
    if (opts.attach) cfg.planetcnc.attach = true;

    mpgd::logInfo("mpgd starting (config=%s, profile=%s, gui=%s, attach=%s, sniff=%s)",
                  opts.configPath.c_str(),
                  cfg.planetcnc.profile.empty() ? "<default>" : cfg.planetcnc.profile.c_str(),
                  opts.noGui ? "headless" : "yes",
                  opts.attach ? "yes" : "no",
                  opts.sniff ? "yes" : "no");

    if (hid_init() != 0) {
        mpgd::logError("hid_init failed");
        return 1;
    }

    mpgd::SharedState state;

    // Install Ctrl+C / SIGTERM handlers before the (up to 30 s) TNG start-up
    // wait so an early interrupt is honored.
    mpgd::Daemon::installSignalHandlers(state.shutdown);

    // --- TNG API (unless sniffing) ----------------------------------------
    mpgd::TngApi api;
    std::thread tngThread;
    std::atomic<bool> tngExited{false};
    if (!opts.sniff) {
        std::string err;
        if (!api.load(cfg.planetcnc.libPath, err)) {
            mpgd::logError("TNG API unavailable: %s", err.c_str());
            hid_exit();
            return 1;
        }

        if (opts.attach) {
            if (!api.isRunningExt()) {
                mpgd::logWarn("attach mode: no external TNG process detected; "
                              "button/display commands may have no effect");
            }
            state.jogEnabled = false;
            mpgd::logWarn("attach mode: jogging disabled (Jog is not available "
                          "through the external pipe interface)");
        } else {
            // Run() blocks for the lifetime of TNG: it runs the TNG message
            // loop on the calling thread and returns only after Exit(). Run it
            // on a dedicated thread and drive the API from the worker threads.
            tngThread = std::thread([&] {
                if (cfg.planetcnc.profile.empty()) {
                    api.run(opts.noGui);
                } else {
                    api.runProfile(opts.noGui, cfg.planetcnc.profile);
                }
                tngExited.store(true);
            });

            // Wait for TNG initialization (init callback or IsInitialized).
            int waited = 0;
            while (!state.shutdown.load() && !api.isInitialized() && waited < 30000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                waited += 100;
            }
            if (state.shutdown.load()) {
                mpgd::logInfo("shutdown requested during TNG start-up");
            } else if (api.isInitialized()) {
                mpgd::logInfo("TNG initialized");
                // Enable axis motors (M10 P1): PlanetCNC jog returns ok but
                // does not move the axes while the motor enable signal is off.
                api.startCode("M10 P1");
            } else {
                mpgd::logWarn("TNG did not report initialized within 30s; "
                              "continuing anyway");
            }
        }
    }

    // --- Components --------------------------------------------------------
    mpgd::ButtonHandler buttonHandler(api, state, cfg);
    mpgd::JogController jogController(api, state, cfg.jogging);
    mpgd::DisplayUpdater displayUpdater(api, state, cfg);
    mpgd::StateReader stateReader(api, state);

    mpgd::usb::HidDevice readDevice;
    mpgd::usb::HidDevice writeDevice;
    mpgd::ButtonQueue buttonQueue;
    mpgd::XhcPendant pendant(state, buttonQueue, cfg.polling,
                             cfg.device.verifyChecksum);

    // usb_hz / display_hz / jog_hz are validated to 1..1000 at config load.
    int jogPeriodMs = 1000 / cfg.polling.jogHz;
    int displayPeriodMs = 1000 / cfg.polling.displayHz;

    mpgd::UsbPollThread usbThread(state, readDevice, writeDevice, pendant,
                                  cfg.device, cfg.polling, opts.sniff);
    mpgd::ButtonThread buttonThread(state, buttonQueue, buttonHandler);
    mpgd::JogThread jogThread(state, jogController, jogPeriodMs);
    mpgd::DisplayThread displayThread(state, writeDevice, displayUpdater,
                                      stateReader, displayPeriodMs);

    // --- Start threads -----------------------------------------------------
    std::vector<std::thread> threads;
    threads.emplace_back([&] { usbThread.run(); });
    if (!opts.sniff) {
        threads.emplace_back([&] { buttonThread.run(); });
        threads.emplace_back([&] { jogThread.run(); });
        threads.emplace_back([&] { displayThread.run(); });
    }

    mpgd::logInfo("mpgd running; press Ctrl+C to stop");
    mpgd::Daemon::waitForShutdown(state.shutdown);

    // --- Graceful shutdown --------------------------------------------------
    mpgd::logInfo("shutting down...");
    state.shutdown.store(true);
    // JogThread::run() calls jogController.stopNow() itself on exit; calling
    // it here as well would race with a tick() still running on that thread.

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    if (!opts.sniff && !opts.attach) {
        api.exitTngForce();
        // Run() should return after ExitForce; give it a bounded grace period
        // and then let the OS clean up the TNG thread if it is still winding
        // down (graceful shutdown semantics are finalized in AC-06).
        for (int i = 0; i < 50 && !tngExited.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (tngThread.joinable()) {
            if (tngExited.load()) {
                tngThread.join();
            } else {
                // The TNG thread is still executing inside the vendor library
                // and references `api`. Returning from main would run
                // ~TngApi (which unloads the library under that thread) and
                // destroy the objects it captured. Terminate the process
                // without running destructors instead.
                mpgd::logWarn("TNG did not exit within 5s; forcing process exit");
                mpgd::Logger::shutdown();
                hid_exit();
                std::quick_exit(0);
            }
        }
    }

    mpgd::Logger::shutdown();
    hid_exit();
    return 0;
}
