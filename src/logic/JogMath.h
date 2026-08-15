#pragma once

#include <algorithm>

namespace mpgd {

// SAFE-05: the pendant reports an int8 wheel delta per packet; accumulated
// counts are clamped to a sane bound to avoid pathological values.
constexpr int kMaxAccumulatedCounts = 32767;

inline int clampAccumulatedCounts(int counts) {
    if (counts > kMaxAccumulatedCounts) return kMaxAccumulatedCounts;
    if (counts < -kMaxAccumulatedCounts) return -kMaxAccumulatedCounts;
    return counts;
}

// Signed jog distance in millimetres for a given accumulated count and the
// current step size (mm per wheel count).
inline double jogDistanceMm(int counts, double stepSize) {
    return static_cast<double>(clampAccumulatedCounts(counts)) * stepSize;
}

} // namespace mpgd
