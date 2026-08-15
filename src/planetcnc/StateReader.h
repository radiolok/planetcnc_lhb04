#pragma once

#include "logic/SharedState.h"

namespace mpgd {

class ITngApi;

// Reads position/status information from the TNG API and publishes it into
// the shared state (called from the display thread).
class StateReader {
public:
    StateReader(ITngApi& api, SharedState& state) : api_(api), state_(state) {}

    // Reads a snapshot of the controller state. Returns false if the TNG API
    // is not initialized.
    bool read();

private:
    ITngApi& api_;
    SharedState& state_;
};

} // namespace mpgd
