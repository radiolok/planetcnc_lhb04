#include "utils/Logger.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

namespace mpgd {

namespace {
std::shared_ptr<spdlog::logger> g_logger;
}

bool Logger::init(const std::string& level, const std::string& file, std::string& error) {
    error.clear();
    // spdlog::level::from_str() maps any unknown string to "off", which would
    // silently disable logging.
    spdlog::level::level_enum lvl = spdlog::level::from_str(level);
    if (lvl == spdlog::level::off && level != "off") {
        error = "unknown log level '" + level + "'; using info";
        lvl = spdlog::level::info;
    }
    std::vector<spdlog::sink_ptr> sinks;

    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_level(lvl);
    sinks.push_back(console);

    if (!file.empty()) {
        try {
            auto fsink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(file, true);
            fsink->set_level(lvl);
            sinks.push_back(fsink);
        } catch (const std::exception& e) {
            if (!error.empty()) error += "; ";
            error +=
                "cannot open log file '" + file + "' (" + e.what() + "); logging to console only";
        }
    }

    g_logger = std::make_shared<spdlog::logger>("mpgd", sinks.begin(), sinks.end());
    g_logger->set_level(lvl);
    g_logger->flush_on(spdlog::level::warn);
    spdlog::flush_every(std::chrono::seconds(1));
    spdlog::set_default_logger(g_logger);
    return error.empty();
}

void Logger::shutdown() {
    spdlog::shutdown();
}

namespace {

void vlog(spdlog::level::level_enum lvl, const char* fmt, va_list args) {
    if (!g_logger) {
        // Fallback before Logger::init.
        char buf[1024];
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        std::fputs(buf, stderr);
        std::fputc('\n', stderr);
        return;
    }
    char buf[2048];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    g_logger->log(lvl, buf);
}

} // namespace

void logTrace(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vlog(spdlog::level::trace, fmt, a);
    va_end(a);
}
void logDebug(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vlog(spdlog::level::debug, fmt, a);
    va_end(a);
}
void logInfo(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vlog(spdlog::level::info, fmt, a);
    va_end(a);
}
void logWarn(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vlog(spdlog::level::warn, fmt, a);
    va_end(a);
}
void logError(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vlog(spdlog::level::err, fmt, a);
    va_end(a);
}

} // namespace mpgd
