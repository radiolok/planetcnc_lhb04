#pragma once

#include "logic/SharedState.h"

namespace mpgd {

class TngApi;

// Reads position/status information from the TNG API and publishes it into
// the shared state (called from the display thread).
class StateReader {
public:
    StateReader(TngApi& api, SharedState& state) : api_(api), state_(state) {}

    // Reads a snapshot of the controller state. Returns false if the TNG API
    // is not initialized.
    bool read();

private:
    TngApi& api_;
    SharedState& state_;
};

} // namespace mpgd
