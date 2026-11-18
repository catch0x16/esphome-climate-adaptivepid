// Native test of the burst learner. No ESPHome needed:
//   uv run python tests/run_native.py
#include <cmath>
#include <cstdio>

#include "burst_learner.h"

using namespace workflow::start;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static bool near(float a, float b, float tolerance = 0.01f) { return std::fabs(a - b) <= tolerance; }

static const uint32_t S = 1000;
static const uint32_t MIN = 60 * S;

// One run: START at t0 with the room at r0, RUN, stop after run_ms with the
// room at r_stop, then a coast up to r_peak `coast_ms` later and back down.
static uint32_t burst(BurstLearner &l, uint32_t t0, float r0, uint32_t run_ms, float r_stop, uint32_t coast_ms,
                      float r_peak, bool shared = false, bool cut = false) {
  uint32_t t = t0;
  l.update(t, true, HeadState::START, true, r0, shared);
  t += 10 * S;
  for (; t < t0 + run_ms; t += 10 * S) {
    const float f = static_cast<float>(t - t0) / run_ms;
    l.update(t, true, HeadState::RUN, true, r0 + f * (r_stop - r0), shared);
  }
  const uint32_t stop = t;
  for (; t <= stop + coast_ms; t += 10 * S) {
    const float f = static_cast<float>(t - stop) / coast_ms;
    if (cut && t > stop + coast_ms / 2) {
      l.update(t, true, HeadState::PARKED, false, r_stop + f * (r_peak - r_stop), shared);
      return t;
    }
    l.update(t, true, HeadState::IDLE, true, r_stop + f * (r_peak - r_stop), shared);
  }
  for (int i = 0; i < 30; i++, t += 10 * S) {
    l.update(t, true, HeadState::IDLE, true, r_peak - 0.02f * i, shared);
  }
  return t;
}

int main() {
  {
    BurstLearner l;
    check(std::isnan(l.gain_alone()), "nothing learned: no gain");
    check(near(l.coast(false), 0.3f) && near(l.coast(true), 0.6f), "nothing learned: default coasts");
    check(l.coast_time_ms() == 5 * MIN, "nothing learned: default coast time");
    // Night of the 16th, alone: start at -0.28, stop at +0.33, peak +0.61 (gain 0.89, coast 0.28).
    uint32_t t = 0;
    for (int i = 0; i < 3; i++) {
      t = burst(l, t, 20.0f, 5 * MIN, 20.61f, 2 * MIN, 20.89f) + MIN;
    }
    check(near(l.gain_alone(), 0.89f, 0.02f), "gain alone learned after 3 runs");
    check(near(l.coast(false), 0.28f, 0.02f), "coast alone learned");
    check(near(l.coast_time_ms() / 1000.0f, 120.0f, 15.0f), "coast time learned");
    check(near(l.coast(true), 0.6f), "shared coast still the default");
    // Shared runs coast further and are kept apart.
    for (int i = 0; i < 3; i++) {
      t = burst(l, t, 20.0f, 5 * MIN, 20.61f, 3 * MIN, 21.4f, true) + MIN;
    }
    check(near(l.coast(true), 0.79f, 0.02f), "coast shared learned separately");
    check(near(l.gain_alone(), 0.89f, 0.02f), "gain alone unchanged by shared runs");
  }
  {
    // A coast cut short by a power-off is discarded.
    BurstLearner l;
    burst(l, 0, 20.0f, 5 * MIN, 20.6f, 4 * MIN, 21.3f, false, true);
    check(l.state().gain_count[0] == 0, "cut coast: nothing recorded");
  }
  {
    // A START the unit never followed is not a run.
    BurstLearner l;
    l.update(0, true, HeadState::START, true, 20.0f, false);
    l.update(60 * S, true, HeadState::IDLE, true, 20.0f, false);
    for (uint32_t t = 70 * S; t < 10 * MIN; t += 10 * S) l.update(t, true, HeadState::IDLE, true, 20.0f, false);
    check(l.state().gain_count[0] == 0, "start timed out: nothing recorded");
  }
  {
    // State round-trips (persistence).
    BurstLearner a;
    uint32_t t = 0;
    for (int i = 0; i < 3; i++) t = burst(a, t, 20.0f, 5 * MIN, 20.6f, 2 * MIN, 20.9f) + MIN;
    BurstLearner b;
    b.set_state(a.state());
    check(near(b.gain_alone(), a.gain_alone()) && b.coast_time_ms() == a.coast_time_ms(), "state restores");
  }

  std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
