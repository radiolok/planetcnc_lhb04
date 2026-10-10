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

// printf-style helpers. GCC and Clang check the arguments against `fmt`.
#if defined(__GNUC__) || defined(__clang__)
#define MPGD_PRINTF_FORMAT(fmtIdx, argIdx) \
    __attribute__((format(printf, fmtIdx, argIdx)))
#else
#define MPGD_PRINTF_FORMAT(fmtIdx, argIdx)
#endif

void logTrace(const char* fmt, ...) MPGD_PRINTF_FORMAT(1, 2);
void logDebug(const char* fmt, ...) MPGD_PRINTF_FORMAT(1, 2);
void logInfo(const char* fmt, ...) MPGD_PRINTF_FORMAT(1, 2);
void logWarn(const char* fmt, ...) MPGD_PRINTF_FORMAT(1, 2);
void logError(const char* fmt, ...) MPGD_PRINTF_FORMAT(1, 2);

} // namespace mpgd
