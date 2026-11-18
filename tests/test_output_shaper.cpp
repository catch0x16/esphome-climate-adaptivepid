// Native test of the PID output shaper. No ESPHome needed:
//   uv run python tests/run_native.py
#include <cmath>
#include <cstdio>

#include "output_shaper.h"

using workflow::pid::OutputShaper;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static bool near(float a, float b) { return std::fabs(a - b) <= 0.001f; }

static const uint32_t S = 1000;
static const uint32_t MIN = 60 * S;

int main() {
  {
    OutputShaper shaper;
    check(!shaper.enabled(), "off: disabled by default");
    check(near(shaper.shape(20.5f, true, true, true, 20.0f, 0), 20.5f), "off: passes the setpoint through");
  }
  {
    // Hailee's room, 21:57: started with the device seeing 20.0 and the setpoint at 20.0.
    OutputShaper shaper;
    shaper.configure(0.5f, 0.0f);
    check(near(shaper.shape(20.47f, true, true, false, 20.0f, 0), 20.47f), "backoff: not running, passes through");
    check(near(shaper.shape(20.47f, true, true, true, 20.0f, 20 * S), 19.5f),
          "backoff: once running, held 0.5 below the start temperature");
    check(shaper.limiting(), "backoff: reports limiting");
    check(near(shaper.shape(20.47f, true, true, true, 20.5f, 6 * MIN), 19.5f),
          "backoff: stays on the start temperature when the device's view rises");
    check(near(shaper.shape(19.20f, true, true, true, 20.5f, 7 * MIN), 19.20f), "backoff: lower requests pass");
    check(!shaper.limiting(), "backoff: not limiting a lower request");
    check(near(shaper.shape(20.47f, true, true, false, 20.5f, 8 * MIN), 20.47f), "backoff: released once stopped");
    check(near(shaper.shape(20.47f, true, true, true, 20.0f, 20 * MIN), 19.5f),
          "backoff: a new run takes a new start temperature");
  }
  {
    // Cooling mirrors it.
    OutputShaper shaper;
    shaper.configure(0.5f, 0.0f);
    check(near(shaper.shape(23.2f, false, true, true, 24.0f, 0), 24.5f),
          "backoff cooling: held 0.5 above the start temperature");
    check(near(shaper.shape(25.0f, false, true, true, 23.5f, MIN), 25.0f), "backoff cooling: higher requests pass");
  }
  {
    OutputShaper shaper;
    shaper.configure(0.0f, 0.1f / 60.0f);  // 0.1 C per minute
    check(near(shaper.shape(19.5f, true, true, false, 20.0f, 0), 19.5f), "rate: first setpoint passes");
    check(near(shaper.shape(20.5f, true, true, false, 20.0f, MIN), 19.6f), "rate: a rise is limited to 0.1 per minute");
    check(near(shaper.shape(20.5f, true, true, false, 20.0f, 3 * MIN), 19.8f), "rate: keeps rising at 0.1 per minute");
    check(near(shaper.shape(18.0f, true, true, false, 20.0f, 3 * MIN + 2 * S), 18.0f), "rate: a drop is immediate");
    check(near(shaper.shape(20.0f, false, true, false, 20.0f, 4 * MIN), 20.0f), "rate: mode change starts afresh");
    check(near(shaper.shape(19.0f, false, true, false, 20.0f, 5 * MIN), 19.9f),
          "rate cooling: a drop (more cooling) is limited");
    check(near(shaper.shape(22.0f, false, true, false, 20.0f, 5 * MIN + 2 * S), 22.0f),
          "rate cooling: a rise (less cooling) is immediate");
  }
  {
    OutputShaper shaper;
    shaper.configure(0.5f, 0.1f / 60.0f);
    shaper.shape(19.0f, true, true, false, 20.0f, 0);
    check(near(shaper.shape(21.0f, true, false, false, 20.0f, MIN), 21.0f), "power off: passes and forgets");
    check(near(shaper.shape(21.0f, true, true, false, 20.0f, 2 * MIN), 21.0f), "power on again: first setpoint passes");
  }

  std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
