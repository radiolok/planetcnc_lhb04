#include "test_framework.h"
#include "logic/JogMath.h"
#include "logic/SharedState.h"

using namespace mpgd;

static void test_clamp() {
    CHECK_EQ(clampAccumulatedCounts(0), 0);
    CHECK_EQ(clampAccumulatedCounts(100), 100);
    CHECK_EQ(clampAccumulatedCounts(-100), -100);
    CHECK_EQ(clampAccumulatedCounts(kMaxAccumulatedCounts), kMaxAccumulatedCounts);
    CHECK_EQ(clampAccumulatedCounts(kMaxAccumulatedCounts + 5000), kMaxAccumulatedCounts);
    CHECK_EQ(clampAccumulatedCounts(-(kMaxAccumulatedCounts + 1)), -kMaxAccumulatedCounts);
}

static void test_jog_distance() {
    // 1 click at 0.001 mm/click.
    CHECK_NEAR(jogDistanceMm(1, 0.001), 0.001, 1e-9);
    // 10 clicks at 0.01 mm/click.
    CHECK_NEAR(jogDistanceMm(10, 0.01), 0.1, 1e-9);
    // negative direction.
    CHECK_NEAR(jogDistanceMm(-3, 0.1), -0.3, 1e-9);
    // 1.0 mm/click.
    CHECK_NEAR(jogDistanceMm(5, 1.0), 5.0, 1e-9);
}

static void test_accumulation() {
    SharedState state;
    // Simulate wheel deltas arriving over several packets (int8 each).
    for (int8_t d : {int8_t(10), int8_t(-3), int8_t(7)}) {
        state.pendant.jogCounts.fetch_add(static_cast<int>(d));
    }
    CHECK_EQ(state.pendant.jogCounts.load(), 14);

    int drained = state.pendant.jogCounts.exchange(0);
    CHECK_EQ(drained, 14);
    CHECK_EQ(state.pendant.jogCounts.load(), 0);
}

static void test_axis_mapping() {
    SharedState state;
    state.pendant.axisCode = xhc::kAxisX;
    CHECK_EQ(state.snapshot().selectedAxis(), 0);
    state.pendant.axisCode = xhc::kAxisY;
    CHECK_EQ(state.snapshot().selectedAxis(), 1);
    state.pendant.axisCode = xhc::kAxisZ;
    CHECK_EQ(state.snapshot().selectedAxis(), 2);
    state.pendant.axisCode = xhc::kAxisA;
    CHECK_EQ(state.snapshot().selectedAxis(), 3);
    state.pendant.axisCode = xhc::kAxisOff;
    CHECK_EQ(state.snapshot().selectedAxis(), -1);
    CHECK(state.snapshot().axisOff());
    state.pendant.axisCode = xhc::kAxisFeed;
    CHECK(state.snapshot().feedOverrideSelected());
    state.pendant.axisCode = xhc::kAxisSpindle;
    CHECK(state.snapshot().spindleOverrideSelected());
}

int main() {
    test_clamp();
    test_jog_distance();
    test_accumulation();
    test_axis_mapping();
    return tfw::summary("test_jog_controller");
}
