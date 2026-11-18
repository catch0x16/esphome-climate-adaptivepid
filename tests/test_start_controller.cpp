// Native test of start control. No ESPHome needed:
//   uv run python tests/run_native.py
#include <cmath>
#include <cstdio>

#include "start_controller.h"

using namespace workflow::start;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static bool near(float a, float b) { return std::fabs(a - b) <= 0.001f; }

static const uint32_t S = 1000;
static const uint32_t MIN = 60 * S;

// Hailee's room: target 20.28, the unit sees 20.0.
static StartInput idle_input(uint32_t now, float demand) {
  StartInput in;
  in.now_ms = now;
  in.heating = true;
  in.powered = true;
  in.operating = false;
  in.device_view = 20.0f;
  in.room = 20.0f;
  in.target = 20.28f;
  in.demand = demand;
  in.coast = 0.3f;
  return in;
}

// Hysteresis band: back on at target - 0.25, parked at target + 0.4.
static StartController make() {
  StartController c;
  StartConfig cfg;
  cfg.start_threshold = 0.5f;
  cfg.on_threshold = 0.25f;
  cfg.off_threshold = 0.4f;
  cfg.min_cycle_ms = 60 * S;
  c.configure(cfg);
  return c;
}

int main() {
  {
    StartController c = make();
    StartDecision d = c.update(idle_input(0, 0.3f));
    check(d.state == HeadState::IDLE, "idle: starts idle when powered and not running");
    check(near(d.setpoint, 19.0f), "idle: setpoint two steps below the unit's view (no timer restart)");
    check(!d.freeze_integral, "idle: integral accumulates");
    // Last night's starts came at demand 0.18 (PID 20.10 vs target 20.28): no start now.
    d = c.update(idle_input(2 * S, 0.29f));
    check(d.state == HeadState::IDLE, "idle: a small demand does not start the unit");
    d = c.update(idle_input(4 * S, 0.5f));
    check(d.state == HeadState::START && d.transition == Transition::STARTED, "start: demand reaches the threshold");
    check(near(d.setpoint, 20.0f), "start: setpoint at the unit's view");
    check(d.freeze_integral, "start: integral frozen");
    StartInput in = idle_input(14 * S, 0.5f);
    in.operating = true;
    in.coast = 0.0f;  // no early stop here: the unit stops by itself
    d = c.update(in);
    check(d.state == HeadState::RUN && d.transition == Transition::RUNNING, "run: the unit reports running");
    check(near(d.setpoint, 19.5f), "run: one step below the view at the start");
    in.now_ms = 3 * MIN;
    in.device_view = 20.5f;  // the unit's view moved up a step
    in.room = 20.2f;
    d = c.update(in);
    check(near(d.setpoint, 19.5f), "run: stays on the start view as the unit's view rises (unit stops itself)");
    in.now_ms = 3 * MIN + 10 * S;
    in.operating = false;
    d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_BY_UNIT, "run: the unit stopped by itself");
    check(near(d.setpoint, 19.5f), "idle after stop: two steps below the new view, never one");
    in = idle_input(3 * MIN + 40 * S, 0.6f);
    d = c.update(in);
    check(d.state == HeadState::IDLE, "idle: no start within the minimum cycle after a stop");
    in.now_ms = 5 * MIN;
    d = c.update(in);
    check(d.state == HeadState::START, "idle: starts again after the minimum cycle");
  }
  {
    // Early stop: the predicted peak (room + coast) reaches the target.
    StartController c = make();
    c.update(idle_input(0, 0.6f));
    StartInput in = idle_input(10 * S, 0.6f);
    in.operating = true;
    c.update(in);
    in.now_ms = 60 * S;
    in.room = 20.0f;  // +0.3 coast would land at 20.30 >= 20.28
    StartDecision d = c.update(in);
    check(d.state == HeadState::RUN, "early stop: not before the minimum run");
    in.now_ms = 3 * MIN;
    d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_EARLY, "early stop: predicted peak at target");
    check(near(d.setpoint, 19.0f), "early stop: idle setpoint stops the compressor");
    in.now_ms = 3 * MIN + 20 * S;  // the unit still reports running for a few seconds
    d = c.update(in);
    check(d.state == HeadState::IDLE, "early stop: a lagging 'running' report is not an unrequested start");
    in.now_ms = 5 * MIN;
    d = c.update(in);
    check(d.state == HeadState::RUN && d.transition == Transition::UNREQUESTED_START,
          "unrequested start: running long after a stop is tracked as a run");
  }
  {
    // No start while the room is still rising, while shared heat arrives, or
    // while hysteresis holds past its off threshold.
    StartController c = make();
    StartInput in = idle_input(0, 0.7f);
    in.room_rising = true;
    check(c.update(in).state == HeadState::IDLE, "no start mid-coast (room rising)");
    in.room_rising = false;
    in.shared_heat = true;
    check(c.update(in).state == HeadState::IDLE, "no start while heat arrives from other heads");
    in.shared_heat = false;
    in.hold = true;
    check(c.update(in).state == HeadState::IDLE, "no start while hysteresis holds");
    in.hold = false;
    check(c.update(in).state == HeadState::START, "starts once nothing holds it back");
  }
  {
    // Shared heat during a run stops it.
    StartController c = make();
    c.update(idle_input(0, 0.6f));
    StartInput in = idle_input(10 * S, 0.6f);
    in.operating = true;
    in.room = 19.7f;
    c.update(in);
    in.now_ms = 30 * S;
    in.shared_heat = true;
    StartDecision d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_SHARED, "shared heat stops a run");
  }
  {
    // Park and unpark on the hysteresis band.
    StartController c = make();
    StartInput in = idle_input(0, 0.0f);
    in.room = 20.6f;  // +0.32: inside the band
    in.shared_heat = true;
    StartDecision d = c.update(in);
    check(d.power == PowerRequest::NONE && d.state == HeadState::IDLE,
          "shared heat inside the band: stays powered and takes the leak");
    in.now_ms = 2 * S;
    in.room = 20.7f;  // +0.42: past off_threshold
    d = c.update(in);
    check(d.power == PowerRequest::TURN_OFF && d.transition == Transition::PARK, "park: shared heat past off_threshold");
    in.now_ms = 5 * S;
    in.powered = false;
    d = c.update(in);
    check(d.state == HeadState::PARKED && d.power == PowerRequest::NONE, "parked once the device is off");
    check(d.freeze_integral, "parked: integral frozen");
    check(near(d.setpoint, 19.0f), "parked: the setpoint to switch on with is the idle level");
    in.now_ms = 2 * MIN;
    in.shared_heat = false;
    in.room = 20.1f;  // -0.18
    in.demand = 0.6f;
    d = c.update(in);
    check(d.power == PowerRequest::NONE, "parked: demand alone does not unpark inside the band");
    in.room = 20.0f;  // -0.28: past on_threshold
    d = c.update(in);
    check(d.power == PowerRequest::TURN_ON && d.transition == Transition::UNPARK, "unpark: room past on_threshold");
    in.now_ms = 2 * MIN + 5 * S;
    in.powered = true;
    in.demand = 0.0f;
    d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::UNPARKED, "unparked: idle again");
    // Shared heat past the band again: may not park within the minimum cycle.
    in.shared_heat = true;
    in.room = 20.7f;
    d = c.update(in);
    check(d.power == PowerRequest::NONE, "no park again within the minimum cycle");
    in.now_ms = 4 * MIN;
    d = c.update(in);
    check(d.power == PowerRequest::TURN_OFF, "parks again after the minimum cycle");
  }
  {
    // Without shared heat the controller leaves the cut to hysteresis (and its off_delay).
    StartController c = make();
    StartInput in = idle_input(0, 0.0f);
    in.room = 20.8f;
    check(c.update(in).power == PowerRequest::NONE, "no park past the band without shared heat");
  }
  {
    // A hysteresis cut counts as parked; the controller switches it on again at on_threshold.
    StartController c = make();
    c.update(idle_input(0, 0.0f));
    StartInput in = idle_input(10 * S, 0.0f);
    in.powered = false;
    StartDecision d = c.update(in);
    check(d.state == HeadState::PARKED && d.transition == Transition::PARKED, "hysteresis cut: parked");
    in.now_ms = 2 * MIN;
    in.room = 20.1f;
    in.demand = 0.35f;
    d = c.update(in);
    check(d.power == PowerRequest::NONE, "hysteresis cut: stays off inside the band");
    in.room = 19.9f;
    d = c.update(in);
    check(d.power == PowerRequest::TURN_ON, "hysteresis cut: switched on again at on_threshold");
  }
  {
    // A hysteresis hold (past off_threshold, waiting out off_delay) stops a run.
    StartController c = make();
    c.update(idle_input(0, 0.6f));
    StartInput in = idle_input(10 * S, 0.6f);
    in.operating = true;
    in.coast = 0.0f;
    StartDecision d = c.update(in);
    check(d.state == HeadState::RUN, "hold: running");
    in.now_ms = 30 * S;
    in.hold = true;
    d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_HOLD, "hold: run stopped before the minimum run");
    check(near(d.setpoint, 19.0f), "hold: idle setpoint stops the compressor");
    // A START is abandoned too.
    StartController s = make();
    s.update(idle_input(0, 0.6f));
    in = idle_input(2 * S, 0.6f);
    in.hold = true;
    d = s.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_HOLD, "hold: start abandoned");
  }
  {
    // Cooling: parked once the room is off_threshold below the target.
    StartController c = make();
    StartInput in = idle_input(0, 0.0f);
    in.heating = false;
    in.device_view = 23.0f;
    in.room = 23.0f;
    in.target = 23.5f;
    in.shared_heat = true;
    check(c.update(in).power == PowerRequest::TURN_OFF, "cooling: park past off_threshold below target");
    in.now_ms = 5 * S;
    in.powered = false;
    c.update(in);
    in.now_ms = 2 * MIN;
    in.shared_heat = false;
    in.room = 23.8f;
    check(c.update(in).power == PowerRequest::TURN_ON, "cooling: unpark on_threshold above target");
  }
  {
    // Cooling mirrors heating.
    StartController c = make();
    StartInput in = idle_input(0, 0.6f);
    in.heating = false;
    in.device_view = 24.0f;
    in.room = 24.0f;
    in.target = 23.5f;
    StartDecision d = c.update(in);
    check(d.state == HeadState::START && near(d.setpoint, 24.0f), "cooling: start at the unit's view");
    in.now_ms = 10 * S;
    in.operating = true;
    d = c.update(in);
    check(near(d.setpoint, 24.5f), "cooling: run one step above the start view");
    in.now_ms = 3 * MIN;
    in.room = 23.7f;  // 23.7 - 0.3 coast <= 23.5
    d = c.update(in);
    check(d.transition == Transition::STOPPED_EARLY && near(d.setpoint, 25.0f), "cooling: early stop, idle two steps above");
  }
  {
    // Without the unit's view, the room rounded to the step stands in.
    StartController c = make();
    StartInput in = idle_input(0, 0.0f);
    in.device_view = NAN;
    in.room = 20.3f;
    check(near(c.update(in).setpoint, 19.5f), "no device view: room rounded to the step");
  }

  std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
