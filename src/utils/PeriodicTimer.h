#pragma once

#include <chrono>
#include <thread>

namespace mpgd {

// Fixed-rate loop pacing. Sleeps until the next deadline (sleep_until) rather
// than for a whole period after the work (sleep_for), so the work time does
// not add to the period and the rate does not drift. When the loop falls
// more than one period behind it re-anchors instead of spinning to catch up.
class PeriodicTimer {
public:
    using Clock = std::chrono::steady_clock;

    explicit PeriodicTimer(std::chrono::milliseconds period)
        : period_(period), next_(Clock::now() + period) {}

    void wait() {
        std::this_thread::sleep_until(next_);
        next_ += period_;
        const auto now = Clock::now();
        if (next_ < now) next_ = now + period_;
    }

private:
    std::chrono::milliseconds period_;
    Clock::time_point next_;
};

} // namespace mpgd
