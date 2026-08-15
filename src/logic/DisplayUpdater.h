#pragma once

#include "logic/SharedState.h"

#include <array>
#include <cstdint>

namespace mpgd {

class TngApi;
struct Config;

// Builds the LCD display frame (6 x 8-byte reports) from the shared machine
// state, following the xhc-hb04.cc encoding (see PacketParser).
class DisplayUpdater {
public:
    static constexpr size_t kReportsCount = xhc::kDisplayReportsCount;
    static constexpr size_t kReportSize = xhc::kDisplayReportSize;
    using Report = std::array<uint8_t, kReportSize>;
    using Frame = std::array<Report, kReportsCount>;

    DisplayUpdater(TngApi& api, SharedState& state, const Config& cfg);

    // Builds one display frame into `out`. Returns false when the frame should
    // not be sent (axis rotary OFF and polling.display_always is false).
    bool build(Frame& out);

private:
    TngApi& api_;
    SharedState& state_;
    const Config& cfg_;
};

} // namespace mpgd
