#pragma once

#include <string>

namespace mpgd {

// Thin facade over spdlog so call sites use short, dependency-light helpers.
class Logger {
public:
    // Configures the process-wide logger. `level` is one of:
    // trace|debug|info|warn|error|critical|off. `file` is an optional log file
    // path; when empty, logs go to stdout/stderr only.
    static void init(const std::string& level, const std::string& file);

    static void shutdown();
};

void logTrace(const char* fmt, ...);
void logDebug(const char* fmt, ...);
void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);

} // namespace mpgd
