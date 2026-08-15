#pragma once

#include "config/ConfigManager.h"
#include "logic/SharedState.h"

namespace mpgd {

class ITngApi;

// Converts accumulated MPG wheel counts into TNG Jog()/JogStop() calls and
// feed/spindle override adjustments. Runs on the jog thread (10 ms tick).
class JogController {
public:
    JogController(ITngApi& api, SharedState& state, const JoggingConfig& cfg);

    // One periodic tick. Drains the wheel-count accumulator and issues the
    // appropriate TNG command. Safe to call repeatedly even when the TNG API
    // is not initialized or jogging is disabled.
    void tick();

    // Immediately stops any active jog and clears the accumulator.
    void stopNow();

private:
    int drainCounts();
    void processJog(int axis, int counts);
    void processOverride(bool spindle, int counts);
    void ensureStopped(bool& wasStopped);

    ITngApi& api_;
    SharedState& state_;
    const JoggingConfig& cfg_;
    bool stopped_ = true;
};

} // namespace mpgd
