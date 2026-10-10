#pragma once

#include <string>

namespace mpgd {

// Thin facade over spdlog so call sites use short, dependency-light helpers.
class Logger {
public:
    // Configures the process-wide logger. `level` is one of:
    // trace|debug|info|warn|error|critical|off. `file` is an optional log file
    // path; when empty, logs go to stdout/stderr only.
    // Never throws. Returns false with `error` set when the level is unknown
    // (logging then uses "info") or the log file cannot be opened (logging
    // then goes to the console only).
    static bool init(const std::string& level, const std::string& file,
                     std::string& error);

    static void shutdown();
};

void logTrace(const char* fmt, ...);
void logDebug(const char* fmt, ...);
void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);

} // namespace mpgd
