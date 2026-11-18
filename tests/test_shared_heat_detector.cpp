// Native test of the shared-heat detector. No ESPHome needed:
//   uv run python tests/run_native.py
#include <cmath>
#include <cstdio>

#include "shared_heat_detector.h"

using workflow::start::SharedHeatDetector;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static const uint32_t S = 1000;
static const uint32_t MIN = 60 * S;

// Feed a room temperature ramp every 2 s from `from` to `to` ms.
static void ramp(SharedHeatDetector &d, uint32_t from, uint32_t to, float room0, float per_min, float hum0,
                 float hum_per_min, bool eligible = true, bool heating = true) {
  for (uint32_t t = from; t <= to; t += 2 * S) {
    const float minutes = (t - from) / 60000.0f;
    d.update(t, heating, eligible, room0 + per_min * minutes, hum0 + hum_per_min * minutes);
  }
}

int main() {
  {
    // Hailee's room while another head runs: +0.16 C/min, humidity falling.
    SharedHeatDetector d;
    ramp(d, 0, 90 * S, 20.0f, 0.16f, 58.0f, -0.6f);
    check(!d.local(), "needs a full window of history");
    ramp(d, 92 * S, 3 * MIN, 20.0f + 0.16f * 1.53f, 0.16f, 57.1f, -0.6f);
    check(d.local() && d.active(3 * MIN), "rising room with falling humidity: shared heat");
    check(d.rising(3 * MIN), "rising");
    // The room stops rising: shared heat ends once it has been flat for the window.
    ramp(d, 3 * MIN + 2 * S, 4 * MIN, 20.48f, 0.0f, 56.0f, 0.0f);
    check(d.local(), "still active while the window includes the rise");
    ramp(d, 4 * MIN + 2 * S, 6 * MIN, 20.48f, 0.0f, 56.0f, 0.0f);
    check(!d.local(), "ends once the room has settled");
  }
  {
    // Rising without falling humidity (someone in the room, sun): not shared heat.
    SharedHeatDetector d;
    ramp(d, 0, 3 * MIN, 20.0f, 0.1f, 58.0f, 0.0f);
    check(!d.local(), "humidity steady: not shared heat");
    check(d.rising(3 * MIN), "but the room is rising");
  }
  {
    // Without a humidity reading the temperature alone decides.
    SharedHeatDetector d;
    ramp(d, 0, 3 * MIN, 20.0f, 0.1f, NAN, 0.0f);
    check(d.local(), "no humidity: temperature alone");
  }
  {
    // Slowly cooling room: neither shared heat nor rising.
    SharedHeatDetector d;
    ramp(d, 0, 4 * MIN, 20.5f, -0.02f, 58.0f, 0.1f);
    check(!d.local() && !d.rising(4 * MIN), "cooling room: nothing");
  }
  {
    // Not eligible (running, in its own coast, or parked): never local.
    SharedHeatDetector d;
    ramp(d, 0, 3 * MIN, 20.0f, 0.2f, 58.0f, -1.0f, false);
    check(!d.local(), "not eligible: not detected");
    check(d.rising(3 * MIN), "rising is still reported");
  }
  {
    // Cooling mode mirrors: a falling room is shared cooling.
    SharedHeatDetector d;
    ramp(d, 0, 3 * MIN, 24.0f, -0.1f, 50.0f, 0.0f, true, false);
    check(d.local(), "cooling: falling room is shared cooling");
  }
  {
    // Supervisor hint overrides until it expires.
    SharedHeatDetector d;
    ramp(d, 0, 3 * MIN, 20.0f, 0.0f, 58.0f, 0.0f);
    d.set_hint(true, 3 * MIN, 120 * S);
    check(d.active(3 * MIN + 10 * S), "hint: active");
    check(!d.local(), "hint: local still off");
    check(!d.active(5 * MIN + 1 * S), "hint: expired, local decides");
    d.set_hint(false, 6 * MIN, 120 * S);
    check(!d.active(6 * MIN + 5 * S), "hint: inactive");
  }

  std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
