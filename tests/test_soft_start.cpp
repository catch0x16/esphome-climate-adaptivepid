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

static SoftStart make(float offset = 1.0f, uint32_t hold_ms = 20 * MIN) {
  SoftStartConfig config;
  config.enabled = true;
  config.initial_offset = offset;
  config.hold_ms = hold_ms;
  SoftStart soft;
  soft.configure(config);
  return soft;
}

// One soft start heating to `target` with the device switched on throughout:
// the room rises to `peak` past the target at 15 minutes and then cools. The
// cap is evaluated each minute so the soft start ends on its own.
static bool run_start(SoftStart &soft, float target, float peak) {
  soft.arm(0, true, target, NAN, false, NAN);
  for (uint32_t t = 0; t <= 100 * MIN; t += MIN) {
    soft.cap(t, MAX_OUTPUT);
    const float room = t <= 15 * MIN ? target - 0.3f + (peak + 0.3f) * (t / (15.0f * MIN))
                                     : target + peak - 0.01f * ((t - 15 * MIN) / MIN);
    if (soft.observe(t, true, room, target, true, MAX_OFFSET)) return true;
  }
  return false;
}

int main() {
  {
    // Heating to 20 with an offset of 1 over 20 minutes: 19.0, climbing 0.05 C/min.
    SoftStart soft = make(1.0f, 20 * MIN);
    check(std::isinf(soft.cap(0, MAX_OUTPUT)), "no cap until armed");
    soft.arm(0, true, 20.0f, NAN, false, NAN);
    check(near(20.0f + soft.cap(0, MAX_OUTPUT), 19.0f), "starts offset below the target: 19.0 for 20");
    check(near(20.0f + soft.cap(10 * MIN, MAX_OUTPUT), 19.5f), "halfway through the hold, halfway to the target");
    check(near(20.0f + soft.cap(20 * MIN, MAX_OUTPUT), 20.0f), "reaches the target at the end of the hold");
    check(near(20.0f + soft.cap(26 * MIN, MAX_OUTPUT), 20.3f), "keeps climbing at the same rate past the target");
    check(soft.active(), "still active below the output limit");
    check(std::isinf(soft.cap(30 * MIN, MAX_OUTPUT)) && !soft.active(), "ends once the cap reaches the output limit");
  }
  {
    // The rate follows the offset and the hold.
    SoftStart a = make(1.0f, 7 * MIN);
    SoftStart b = make(1.5f, 30 * MIN);
    check(near(a.climb_rate(true) * 60.0f, 1.0f / 7.0f), "offset 1 over 7 minutes: 1/7 C per minute");
    check(near(b.climb_rate(true) * 60.0f, 0.05f), "offset 1.5 over 30 minutes: 0.05 C per minute");
    SoftStart small = make(0.0f, 10 * MIN);
    small.arm(0, true, 20.0f, NAN, false, NAN);
    check(near(small.cap(0, MAX_OUTPUT), 0.0f) && small.climb_rate(true) > 0.0f,
          "an offset of 0 starts at the target and still climbs");
  }
  {
    // The start does not depend on the room: only on the target and the offset.
    SoftStart a = make();
    SoftStart b = make();
    a.arm(0, true, 20.0f, NAN, false, NAN);
    b.arm(0, true, 20.0f, NAN, false, NAN);
    a.observe(0, true, 19.9f, 20.0f, true, MAX_OFFSET);
    b.observe(0, true, 19.2f, 20.0f, true, MAX_OFFSET);
    check(near(a.cap(MIN, MAX_OUTPUT), b.cap(MIN, MAX_OUTPUT)), "same start whatever the room temperature");
  }
  {
    // A running device is not stopped: the cap starts from where it runs.
    SoftStart soft = make();
    soft.arm(0, true, 20.0f, NAN, true, -0.1f);
    check(near(soft.cap(0, MAX_OUTPUT), -0.1f), "a running device's cap starts at its current output");
  }
  {
    // Learning from overshoot.
    SoftStart soft = make(1.0f);
    check(run_start(soft, 20.0f, 0.85f), "measured 30 minutes after the cap lifts");
    check(near(soft.last_overshoot(), 0.85f, 0.01f), "the measured overshoot is the room's peak past the target");
    check(near(soft.offset(true), 1.0f + 0.5f * (0.85f - 0.25f), 0.01f), "overshoot above 0.25 starts the next one lower");
    check(near(soft.offset(false), 1.0f), "cooling is learned separately");

    SoftStart gentle = make(1.0f);
    run_start(gentle, 20.0f, 0.1f);
    check(near(gentle.offset(true), 0.9f, 0.01f), "overshoot at or below 0.25 starts the next one 0.1 higher");

    SoftStart bounded = make(1.9f);
    run_start(bounded, 20.0f, 2.0f);
    check(near(bounded.offset(true), MAX_OFFSET), "the offset never exceeds max_adjustment_under");
    SoftStart floor = make(0.05f);
    run_start(floor, 20.0f, 0.0f);
    check(near(floor.offset(true), 0.0f), "the offset never goes below 0");
  }
  {
    // Hailee's bedroom on 15 November: switched on with the setpoint 1.5 below
    // the room, the unit reported idle throughout, yet the room rose 1.2 in
    // five minutes and the workflow switched it off. The overshoot counts.
    SoftStart soft = make(1.5f);
    soft.arm(0, true, 20.28f, NAN, false, NAN);
    const float rooms[] = {20.01f, 20.14f, 20.44f, 20.71f, 20.98f, 21.21f};
    bool learned = false;
    uint32_t t = 0;
    for (; t <= 5 * MIN; t += MIN) {
      soft.cap(t, MAX_OUTPUT);
      learned |= soft.observe(t, true, rooms[t / MIN], 20.28f, t < 5 * MIN, MAX_OFFSET);
    }
    soft.cancel();  // switched off at 5 minutes
    for (; t <= 30 * MIN && !learned; t += MIN) {
      learned |= soft.observe(t, true, 21.21f - 0.02f * ((t - 5 * MIN) / MIN), 20.28f, false, MAX_OFFSET);
    }
    check(learned, "a unit reporting idle still has its overshoot measured");
    check(t <= 17 * MIN, "measured 10 minutes after the unit is switched off");
    check(near(soft.last_overshoot(), 0.93f, 0.01f), "Hailee's overshoot of 0.93 is measured");
    check(soft.offset(true) > 1.5f, "and the next soft start begins lower");
  }
  {
    // Measurements that are not this soft start's outcome are dropped.
    SoftStart soft = make();
    soft.arm(0, true, 20.0f, NAN, false, NAN);
    soft.observe(MIN, true, 19.8f, 20.0f, true, MAX_OFFSET);
    soft.observe(2 * MIN, true, 20.5f, 21.0f, true, MAX_OFFSET);  // target changed
    bool learned = false;
    for (uint32_t t = 3 * MIN; t < 100 * MIN; t += MIN) learned |= soft.observe(t, true, 20.6f, 21.0f, false, MAX_OFFSET);
    check(!learned && near(soft.offset(true), 1.0f), "a target change drops the measurement");

    SoftStart rearmed = make();
    rearmed.arm(0, true, 20.0f, NAN, false, NAN);
    rearmed.observe(MIN, true, 20.9f, 20.0f, true, MAX_OFFSET);
    rearmed.arm(2 * MIN, true, 20.0f, NAN, false, NAN);  // new demand before the soft start ended
    check(near(rearmed.offset(true), 1.0f), "new demand before the soft start ended drops the measurement");

    SoftStart off = make();
    off.arm(0, true, 20.0f, NAN, false, NAN);
    off.observe(MIN, true, 19.8f, 20.0f, true, MAX_OFFSET);
    off.set_enabled(false);
    check(!off.active() && std::isinf(off.cap(2 * MIN, MAX_OUTPUT)), "disabling cancels a running soft start");
    learned = false;
    for (uint32_t t = 2 * MIN; t < 100 * MIN; t += MIN) learned |= off.observe(t, true, 20.8f, 20.0f, false, MAX_OFFSET);
    check(!learned && near(off.offset(true), 1.0f), "disabling drops the measurement");
    off.arm(0, true, 20.0f, NAN, false, NAN);
    check(!off.active(), "a disabled soft start does not arm");
  }
  {
    // The device setpoint under a soft start: the PID with the cap, heating to
    // 20 from 19.7. It starts at 19.0 and never rises faster than the climb.
    SoftStart soft = make(1.0f, 20 * MIN);
    AdaptivePID pid(0.9f, 0.0003f, 0.0f);
    pid.set_output_limits(-2.0f, MAX_OUTPUT);
    pid.set_target(20.0f, true);
    soft.arm(0, true, 20.0f, NAN, false, NAN);
    float previous = NAN;
    float first = NAN;
    float largest_rise = 0.0f;
    for (uint32_t t = 0; t <= 40 * MIN; t += 2000) {
      const float room = 19.7f + 0.4f * (t / (40.0f * MIN));
      pid.set_output_cap(soft.cap(t, MAX_OUTPUT));
      const float setpoint = pid.update(room, t, true);
      if (std::isnan(first)) first = setpoint;
      if (std::isfinite(previous) && setpoint - previous > largest_rise) largest_rise = setpoint - previous;
      previous = setpoint;
    }
    check(near(first, 19.0f), "device setpoint starts at target - offset");
    // 2 s at 0.05 C/min is 0.0017 C.
    check(largest_rise <= 0.05f / 30.0f + 0.0005f, "no jump: the setpoint never rises faster than the climb");
    check(previous < 20.0f + MAX_OUTPUT, "the PID takes over as the room nears the target");
  }
  {
    // Heat arrives once the room is 0.15 above its lowest reading since the start.
    SoftStart soft = make(1.0f, 20 * MIN);
    soft.arm(0, true, 20.0f, 19.70f, false, NAN);
    check(!soft.heat_arrived(19.60f), "falling: no heat yet");
    check(!soft.heat_arrived(19.50f), "lower still: no heat yet");
    check(!soft.heat_arrived(19.60f), "0.10 above the lowest reading is within sensor jitter");
    check(soft.heat_arrived(19.66f), "0.16 above the lowest reading: heat has arrived");
  }
  {
    // Hand-off: the PID continues from the cap. Its own output is +0.5 (kp
    // times a large shortfall); the cap is -0.5, so it gets a bias of -1.0.
    SoftStart soft = make(1.0f, 20 * MIN);
    soft.arm(0, true, 20.0f, 19.5f, false, NAN);
    const float bias = soft.hand_off(10 * MIN, MAX_OUTPUT, 0.5f);
    check(near(bias, -1.0f), "hand-off bias: cap - PID output");
    check(soft.phase() == SoftStart::Phase::HANDOFF && std::isinf(soft.cap(10 * MIN, MAX_OUTPUT)),
          "after the hand-off there is no cap, only the bias");
    check(near(soft.bias(10 * MIN), -1.0f), "the bias starts at the hand-off value");
    check(near(soft.bias(20 * MIN), -0.5f), "and fades to zero over the hold");
    check(near(soft.bias(30 * MIN), 0.0f) && !soft.active(), "the soft start ends once the bias has faded");

    SoftStart already = make(1.0f, 20 * MIN);
    already.arm(0, true, 20.0f, 19.9f, false, NAN);
    check(near(already.hand_off(10 * MIN, MAX_OUTPUT, -0.8f), 0.0f) && !already.active(),
          "a PID already asking for less than the cap needs no bias");
  }
  {
    // The game-out for Primary: target 19.72, kp 1.05, the room 0.25 short
    // and falling 1 C/h while the unit idles, then rising 5 C/h once heat
    // arrives at 18 minutes. The device setpoint does not jump at the
    // hand-off and then comes down as the room rises, instead of climbing to
    // target + 0.5 as it did without the hand-off.
    SoftStart soft = make(1.0f, 20 * MIN);
    AdaptivePID pid(1.05f, 0.00034f, 0.0f);
    pid.set_output_limits(-2.0f, MAX_OUTPUT);
    pid.set_target(19.72f, true);
    float room = 19.47f;
    soft.arm(0, true, 19.72f, room, false, NAN);
    float previous = NAN, at_handoff = NAN, highest_after = -INFINITY, last = NAN;
    float largest_step = 0.0f;
    uint32_t handoff_t = 0;
    for (uint32_t t = 0; t <= 40 * MIN; t += 2000) {
      room += (t < 18 * MIN ? -1.0f : 5.0f) / 3600.0f * 2.0f;
      if (soft.heat_arrived(room)) {
        soft.hand_off(t, MAX_OUTPUT, pid.raw_offset());
        handoff_t = t;
      }
      pid.set_output_cap(soft.cap(t, MAX_OUTPUT));
      pid.set_output_bias(soft.bias(t));
      const float setpoint = pid.update(room, t, true);
      if (std::isfinite(previous) && std::fabs(setpoint - previous) > largest_step) largest_step = std::fabs(setpoint - previous);
      if (handoff_t != 0 && t == handoff_t) at_handoff = setpoint;
      if (handoff_t != 0 && t > handoff_t && setpoint > highest_after) highest_after = setpoint;
      previous = setpoint;
      last = setpoint;
    }
    check(handoff_t > 18 * MIN && handoff_t < 21 * MIN, "hands off about 2 minutes after the room turns");
    check(largest_step < 0.01f, "no jump at the hand-off, or anywhere");
    check(highest_after <= at_handoff + 0.01f, "after the hand-off the setpoint does not climb while the room rises");
    check(last < 19.72f, "with the room past the target the setpoint is below it");
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
