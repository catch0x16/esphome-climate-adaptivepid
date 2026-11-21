// Native test of when continuous start control engages. No ESPHome needed:
//   uv run python tests/run_native.py
#include <cmath>
#include <cstdio>

#include "start_engagement.h"

using namespace workflow::start;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static const uint32_t S = 1000;

static StartEngagement make() {
  StartEngagement e;
  e.configure(60 * S, 300);  // min_cycle_duration 60 s, forecast window 5 min
  return e;
}

int main() {
  {
    StartEngagement e = make();
    check(!e.signal(false, NAN), "no hint, no forecast: no signal");
    check(e.signal(true, NAN), "hint: signal");
    check(e.signal(false, 300.0f), "forecast within the window: signal");
    check(!e.signal(false, 301.0f), "forecast past the window: no signal");
  }
  {
    // Engaged on a hint; released only after the release time, once settled.
    StartEngagement e = make();
    check(e.update(0, false, true) == EngageChange::NONE && !e.engaged(), "starts disengaged");
    check(e.update(2 * S, true, true) == EngageChange::ENGAGED && e.engaged(), "engages on the signal");
    check(e.update(4 * S, true, false) == EngageChange::NONE, "stays engaged while signalled");
    check(e.update(30 * S, false, true) == EngageChange::NONE && e.engaged(), "held through a short gap");
    check(e.update(63 * S, false, false) == EngageChange::NONE && e.engaged(),
          "not released while a run it started is on");
    check(e.update(64 * S, false, true) == EngageChange::RELEASED && !e.engaged(),
          "released once settled, release time after the last signal");
    check(e.update(66 * S, false, true) == EngageChange::NONE, "stays released");
  }
  {
    // A signal during the release time restarts it.
    StartEngagement e = make();
    e.update(0, true, true);
    e.update(50 * S, true, true);
    check(e.update(100 * S, false, true) == EngageChange::NONE, "release timed from the last signal");
    check(e.update(110 * S, false, true) == EngageChange::RELEASED, "released 60 s after the last signal");
  }
  {
    // Time wraps around.
    StartEngagement e = make();
    const uint32_t t = UINT32_MAX - 10 * S;
    e.update(t, true, true);
    check(e.update(t + 30 * S, false, true) == EngageChange::NONE, "across the wrap: held");
    check(e.update(t + 60 * S, false, true) == EngageChange::RELEASED, "across the wrap: released");
  }

  std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
