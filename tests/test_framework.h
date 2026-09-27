#pragma once

// Minimal dependency-free test harness used by the mpgd unit tests.
// Each test executable is a single translation unit, so the inline globals are
// safe.

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tfw {

inline int g_checks = 0;
inline int g_failures = 0;

inline void reportFailure(const char* file, int line, const char* expr) {
    ++g_failures;
    std::printf("  FAIL %s:%d: %s\n", file, line, expr);
}

inline int summary(const char* name) {
    std::printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

} // namespace tfw

#define CHECK(cond)                                                 \
    do {                                                            \
        ++tfw::g_checks;                                            \
        if (!(cond)) tfw::reportFailure(__FILE__, __LINE__, #cond); \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++tfw::g_checks;                                                         \
        if (!((a) == (b))) {                                                     \
            std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
            ++tfw::g_failures;                                                   \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                \
    do {                                                                                     \
        ++tfw::g_checks;                                                                     \
        if (std::fabs((a) - (b)) > (eps)) {                                                  \
            std::printf("  FAIL %s:%d: |%s - %s| > %s\n", __FILE__, __LINE__, #a, #b, #eps); \
            ++tfw::g_failures;                                                               \
        }                                                                                    \
    } while (0)
