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
static constexpr float MAX_OUTPUT = 0.5f;  // max_adjustment_over
static constexpr float MAX_OFFSET = 2.0f;  // max_adjustment_under

static SoftStart make(float offset = 1.5f) {
  SoftStartConfig config;
  config.enabled = true;
  config.initial_offset = offset;
  config.hold_ms = 4 * MIN;
  config.rate_per_s = 0.1f / 60.0f;
  SoftStart soft;
  soft.configure(config);
  return soft;
}

// One soft start heating to `target`: the room reaches `peak` while the device
// runs for 10 minutes, then the measurement settles.
static bool run_start(SoftStart &soft, float target, float peak) {
  uint32_t t = 0;
  soft.arm(t, true, target, false, NAN);
  bool learned = false;
  for (; t <= 40 * MIN && !learned; t += MIN) {
    const bool operating = t >= 2 * MIN && t < 12 * MIN;
    // Rises to the peak at 12 minutes, then cools 0.01 C a minute.
    const float room = t <= 12 * MIN ? target - 0.3f + (peak + 0.3f) * (t / (12.0f * MIN))
                                     : target + peak - 0.01f * ((t - 12 * MIN) / MIN);
    learned = soft.observe(t, true, room, target, operating, MAX_OFFSET);
  }
  return learned;
}

int main() {
  {
    // Heating to 20 with an offset of 1.5: the setpoint starts at 18.5.
    SoftStart soft = make(1.5f);
    check(std::isinf(soft.cap(0, MAX_OUTPUT)), "no cap until armed");
    soft.arm(0, true, 20.0f, false, NAN);
    check(near(20.0f + soft.cap(1 * MIN, MAX_OUTPUT), 18.5f), "starts offset below the target: 18.5 for 20");
    check(near(20.0f + soft.cap(4 * MIN, MAX_OUTPUT), 18.5f), "held for the hold time");
    check(near(20.0f + soft.cap(9 * MIN, MAX_OUTPUT), 19.0f), "then rises at the ramp rate");
    check(near(20.0f + soft.cap(19 * MIN, MAX_OUTPUT), 20.0f), "reaches the target 15 minutes after the hold");
    check(soft.active(), "still active below the output limit");
    check(std::isinf(soft.cap(24 * MIN, MAX_OUTPUT)) && !soft.active(), "ends once the cap reaches the output limit");
  }
  {
    // The start does not depend on the room: only on the target and the offset.
    SoftStart a = make(1.5f);
    SoftStart b = make(1.5f);
    a.arm(0, true, 20.0f, false, NAN);
    b.arm(0, true, 20.0f, false, NAN);
    a.observe(0, true, 19.9f, 20.0f, false, MAX_OFFSET);
    b.observe(0, true, 19.2f, 20.0f, false, MAX_OFFSET);
    check(near(a.cap(1 * MIN, MAX_OUTPUT), b.cap(1 * MIN, MAX_OUTPUT)), "same start whatever the room temperature");
  }
  {
    // A running device is not stopped: the cap starts from where it runs.
    SoftStart soft = make();
    soft.arm(0, true, 20.0f, true, -0.1f);
    check(near(soft.cap(1 * MIN, MAX_OUTPUT), -0.1f), "a running device's cap starts at its current output");
  }
  {
    // Learning from overshoot.
    SoftStart soft = make(1.5f);
    check(run_start(soft, 20.0f, 0.85f), "a soft start is measured once the device has stopped and settled");
    check(near(soft.last_overshoot(), 0.85f, 0.01f), "the measured overshoot is the room's peak past the target");
    check(near(soft.offset(true), 1.5f + 0.5f * (0.85f - 0.25f), 0.01f), "overshoot above 0.25 starts the next one lower");
    check(near(soft.offset(false), 1.5f), "cooling is learned separately");

    SoftStart gentle = make(1.5f);
    run_start(gentle, 20.0f, 0.1f);
    check(near(gentle.offset(true), 1.4f, 0.01f), "overshoot at or below 0.25 starts the next one 0.1 higher");

    SoftStart bounded = make(1.9f);
    run_start(bounded, 20.0f, 2.0f);
    check(near(bounded.offset(true), MAX_OFFSET), "the offset never exceeds max_adjustment_under");
    SoftStart floor = make(0.05f);
    run_start(floor, 20.0f, 0.0f);
    check(near(floor.offset(true), 0.0f), "the offset never goes below 0");
  }
  {
    // Measurements that are not this soft start's outcome are dropped.
    SoftStart soft = make(1.5f);
    soft.arm(0, true, 20.0f, false, NAN);
    soft.observe(1 * MIN, true, 19.8f, 20.0f, true, MAX_OFFSET);
    soft.observe(2 * MIN, true, 20.5f, 21.0f, true, MAX_OFFSET);  // target changed
    bool learned = false;
    for (uint32_t t = 3 * MIN; t < 30 * MIN; t += MIN) learned |= soft.observe(t, true, 20.6f, 21.0f, false, MAX_OFFSET);
    check(!learned && near(soft.offset(true), 1.5f), "a target change drops the measurement");

    SoftStart never = make(1.5f);
    never.arm(0, true, 20.0f, false, NAN);
    for (uint32_t t = 0; t <= 100 * MIN; t += MIN) learned |= never.observe(t, true, 19.5f, 20.0f, false, MAX_OFFSET);
    check(!learned && near(never.offset(true), 1.5f), "a device that never ran teaches nothing");

    SoftStart off = make(1.5f);
    off.arm(0, true, 20.0f, false, NAN);
    off.observe(1 * MIN, true, 19.8f, 20.0f, true, MAX_OFFSET);
    off.set_enabled(false);
    check(!off.active() && std::isinf(off.cap(2 * MIN, MAX_OUTPUT)), "disabling cancels a running soft start");
    learned = false;
    for (uint32_t t = 2 * MIN; t < 30 * MIN; t += MIN) learned |= off.observe(t, true, 20.8f, 20.0f, false, MAX_OFFSET);
    check(!learned && near(off.offset(true), 1.5f), "disabling drops the measurement");
    off.arm(0, true, 20.0f, false, NAN);
    check(!off.active(), "a disabled soft start does not arm");
  }
  {
    // The device setpoint under a soft start: the PID with the cap, heating to
    // 20 from 19.7. It starts at 18.5 and never rises faster than the ramp,
    // across the end of the hold included.
    SoftStart soft = make(1.5f);
    AdaptivePID pid(0.9f, 0.0003f, 0.0f);
    pid.set_output_limits(-2.0f, MAX_OUTPUT);
    pid.set_target(20.0f, true);
    soft.arm(0, true, 20.0f, false, NAN);
    float previous = NAN;
    float first = NAN;
    float largest_rise = 0.0f;
    for (uint32_t t = 0; t <= 30 * MIN; t += 2000) {
      const float room = 19.7f + 0.4f * (t / (30.0f * MIN));
      pid.set_output_cap(soft.cap(t, MAX_OUTPUT));
      const float setpoint = pid.update(room, t, true);
      if (std::isnan(first)) first = setpoint;
      if (std::isfinite(previous) && setpoint - previous > largest_rise) largest_rise = setpoint - previous;
      previous = setpoint;
    }
    check(near(first, 18.5f), "device setpoint starts at target - offset");
    // 2 s at 0.1 C/min is 0.0033 C.
    check(largest_rise <= 0.1f / 30.0f + 0.0005f, "no jump: the setpoint never rises faster than the ramp");
    check(previous < 20.0f + MAX_OUTPUT, "the PID takes over as the room nears the target");
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
    pid.set_output_cap(-1.5f);
    check(near(pid.update(24.5f, 1000, true), 25.5f), "cap: cooling output held offset above the target");
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
