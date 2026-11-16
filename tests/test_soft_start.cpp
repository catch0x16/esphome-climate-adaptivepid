// Native test of the soft start and the PID output cap. No ESPHome needed:
//   c++ -std=c++17 -Wall -Wextra -Icomponents/workflow_climate -o /tmp/test_soft_start \
//     tests/test_soft_start.cpp components/workflow_climate/adaptive_pid.cpp && /tmp/test_soft_start
#include <cmath>
#include <cstdio>

#include "adaptive_pid.h"
#include "soft_start.h"

using workflow::pid::SoftStart;
using workflow::pid::SoftStartConfig;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static bool near(float a, float b, float tolerance = 0.001f) { return std::fabs(a - b) <= tolerance; }

static constexpr uint32_t MIN = 60 * 1000;

static SoftStart make(float start_point = 0.0f) {
  SoftStartConfig config;
  config.enabled = true;
  config.initial_start_point = start_point;
  config.margin = 0.5f;
  config.hold_ms = 4 * MIN;
  config.rate_per_s = 0.1f / 60.0f;
  SoftStart soft;
  soft.configure(config);
  return soft;
}

int main() {
  {
    SoftStart soft = make();
    check(std::isinf(soft.cap(0, 0.5f)), "no cap until armed");
    // Room 0.3 short of the target, start point 0, margin 0.5: the device's
    // call starts at -0.5, which is 0.8 below the target.
    soft.arm(0, true, 0.3f, false, NAN);
    check(near(soft.cap(1 * MIN, 0.5f), -0.8f), "starts margin below the start point, relative to the room");
    check(near(soft.cap(4 * MIN, 0.5f), -0.8f), "held for the hold time");
    check(near(soft.cap(9 * MIN, 0.5f), -0.3f), "then rises at the ramp rate");
    check(soft.active(), "still active below the output limit");
    check(std::isinf(soft.cap(17 * MIN, 0.5f)) && !soft.active(), "ends once the cap reaches the output limit");
  }
  {
    // The start point is learned from where the device actually starts.
    SoftStart soft = make();
    soft.arm(0, true, 0.3f, false, NAN);
    check(soft.on_device_start(true, -0.25f), "a start during a soft start is learned");
    check(near(soft.start_point(true), 0.3f * -0.25f), "start point moves towards the observed call");
    check(!soft.on_device_start(true, -0.25f), "only the first start of a soft start is learned");
    check(near(soft.start_point(false), 0.0f), "cooling is learned separately");
  }
  {
    // A device that starts as soon as it is switched on, wherever the
    // setpoint is, starts during the hold: each soft start then begins lower.
    SoftStart soft = make();
    float previous_cap = INFINITY;
    bool lower_every_time = true;
    for (int i = 0; i < 10; i++) {
      soft.arm(0, true, 0.3f, false, NAN);
      const float cap = soft.cap(0, 0.5f);
      if (!(cap < previous_cap)) lower_every_time = false;
      previous_cap = cap;
      soft.on_device_start(true, cap + 0.3f);  // the call at the start: cap + error
    }
    check(lower_every_time, "a device that starts at once pushes the start lower each time");
    check(soft.start_point(true) >= SoftStart::MIN_START_POINT, "the start point stays inside its range");
  }
  {
    // A running device is not stopped: the cap starts from where it runs.
    SoftStart soft = make();
    soft.arm(0, true, 0.6f, true, -0.1f);
    check(near(soft.cap(1 * MIN, 0.5f), -0.1f), "a running device's cap starts at its current output");
    check(!soft.on_device_start(true, 0.2f), "a start that already happened is not learned");
  }
  {
    SoftStart soft = make();
    soft.arm(0, true, 0.3f, false, NAN);
    soft.set_enabled(false);
    check(!soft.active() && std::isinf(soft.cap(1 * MIN, 0.5f)), "disabling cancels a running soft start");
    soft.arm(0, true, 0.3f, false, NAN);
    check(!soft.active(), "a disabled soft start does not arm");
  }
  {
    // The PID cap: heating, a cap below zero holds the output below the target
    // and the integral does not wind up behind it.
    AdaptivePID pid(1.0f, 0.01f, 0.0f);
    pid.set_output_limits(-2.0f, 2.0f);
    pid.set_target(20.0f, true);
    pid.set_output_cap(-1.0f);
    float out = 0.0f;
    uint32_t t = 1000;
    for (int i = 0; i < 300; i++, t += 2000) out = pid.update(19.7f, t, true);
    check(near(out, 19.0f), "cap: heating output held 1 below the target");
    check(near(pid.integral(), 0.0f), "cap: no integral accumulates while the cap binds");
    pid.set_output_cap(INFINITY);
    out = pid.update(19.7f, t, true);
    check(near(out, 20.3f, 0.01f), "cap removed: plain P output, no jump from a wound-up integral");
  }
  {
    // Cooling: more output is a lower setpoint, so the cap holds it above.
    AdaptivePID pid(1.0f, 0.0f, 0.0f);
    pid.set_output_limits(-2.0f, 2.0f);
    pid.set_target(24.0f, false);
    pid.set_output_cap(-1.0f);
    const float out = pid.update(24.5f, 1000, true);
    check(near(out, 25.0f), "cap: cooling output held 1 above the target");
  }
  {
    // A cap below the output range pins the output at the range.
    AdaptivePID pid(1.0f, 0.0f, 0.0f);
    pid.set_output_limits(-1.0f, 1.0f);
    pid.set_target(20.0f, true);
    pid.set_output_cap(-3.0f);
    check(near(pid.update(19.0f, 1000, true), 19.0f), "cap below the range: output stays at the range");
  }

  std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
