#pragma once

#include <atomic>

namespace mpgd {

// Process-lifecycle helpers (signal handling, graceful shutdown).
//
// v1 runs as a foreground console application; a Windows Service / systemd
// wrapper is intentionally deferred (see plan section 1).
class Daemon {
public:
    // Installs Ctrl+C / SIGINT / SIGTERM handlers that set `shutdown`.
    static bool installSignalHandlers(std::atomic<bool>& shutdown);

    // Blocks until `shutdown` becomes true (used by the main thread).
    static void waitForShutdown(const std::atomic<bool>& shutdown, int pollMs = 100);
};

} // namespace mpgd
