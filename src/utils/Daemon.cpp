#include "utils/Daemon.h"

#include "utils/Logger.h"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <csignal>
#endif

namespace mpgd {

namespace {

std::atomic<bool>* g_shutdown = nullptr;

#if defined(_WIN32)
BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        if (g_shutdown) g_shutdown->store(true);
        return TRUE;
    }
    return FALSE;
}
#else
void posixSignalHandler(int sig) {
    (void)sig;
    if (g_shutdown) g_shutdown->store(true);
}
#endif

} // namespace

bool Daemon::installSignalHandlers(std::atomic<bool>& shutdown) {
    g_shutdown = &shutdown;
#if defined(_WIN32)
    if (!SetConsoleCtrlHandler(ctrlHandler, TRUE)) {
        logError("failed to install console control handler");
        return false;
    }
#else
    std::signal(SIGINT, posixSignalHandler);
    std::signal(SIGTERM, posixSignalHandler);
#endif
    return true;
}

void Daemon::waitForShutdown(const std::atomic<bool>& shutdown, int pollMs) {
    while (!shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }
}

} // namespace mpgd
