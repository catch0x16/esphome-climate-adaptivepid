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

// Hailee's room: target 20.28, room 20.00, the unit reports 20.0.
// `gap` is the PID's output minus the room (O - R).
static StartInput input(uint32_t now, float gap) {
  StartInput in;
  in.now_ms = now;
  in.heating = true;
  in.powered = true;
  in.operating = false;
  in.device_view = 20.0f;
  in.room = 20.0f;
  in.target = 20.28f;
  in.output = in.room + gap;
  return in;
}

// The same, with the room (and the PID's output with it) moved.
static StartInput at_room(StartInput in, float room, float gap) {
  in.room = room;
  in.output = room + gap;
  return in;
}

// Hysteresis band: back on at target - 0.25, parked at target + 0.4.
static StartController make() {
  StartController c;
  StartConfig cfg;
  cfg.on_threshold = 0.25f;
  cfg.off_threshold = 0.4f;
  cfg.min_cycle_ms = 60 * S;
  // Runs are confirmed at once here; confirmation has its own tests below.
  cfg.run_confirm_ms = 0;
  c.configure(cfg);
  return c;
}

// The same, with the default run confirmation.
static StartController make_confirming() {
  StartController c = make();
  StartConfig cfg = c.config();
  cfg.run_confirm_ms = StartConfig{}.run_confirm_ms;
  c.configure(cfg);
  return c;
}

// A head started and confirmed running at `now`.
static StartController running(uint32_t now, float gap) {
  StartController c = make();
  c.update(input(now, gap));
  StartInput in = input(now + 10 * S, gap);
  in.operating = true;
  c.update(in);
  return c;
}

int main() {
  {
    // The levels the PID's output asks for, against the room.
    StartController c = make();
    StartDecision d = c.update(input(0, -0.6f));
    check(d.state == HeadState::IDLE && d.level == Level::OFF, "level: output 0.6 below the room is off");
    check(near(d.setpoint, 19.0f), "off level: two steps below the reported temperature");
    check(!d.freeze_integral, "idle: integral accumulates");
    d = c.update(input(2 * S, -0.49f));
    check(d.level == Level::KEEP && d.state == HeadState::IDLE, "level: 0.49 below the room is the running level");
    check(near(d.setpoint, 19.5f), "running level while stopped: one step below the reported temperature");
    d = c.update(input(4 * S, -0.01f));
    check(d.level == Level::KEEP && d.state == HeadState::IDLE, "level: just below the room, no start");
    d = c.update(input(6 * S, 0.0f));
    check(d.level == Level::START && d.state == HeadState::START && d.transition == Transition::STARTED,
          "start: the output reaches the room");
    check(near(d.setpoint, 20.0f), "start level: room 20.00 -> 20.0");
    check(!d.freeze_integral, "start: integral accumulates");
  }
  {
    // The start level is the step at or above the room, not the reported
    // temperature (2026-11-19 19:44: room 20.15, reported 20.0, no start at 20.0).
    StartController a = make();
    check(near(a.update(at_room(input(0, 0.2f), 20.08f, 0.2f)).setpoint, 20.5f), "start level: room 20.08 -> 20.5");
    StartController b = make();
    check(near(b.update(at_room(input(0, 0.2f), 19.92f, 0.2f)).setpoint, 20.0f), "start level: room 19.92 -> 20.0");
    StartController e = make();
    check(near(e.update(at_room(input(0, 0.2f), 20.0f, 0.2f)).setpoint, 20.0f), "start level: room 20.00 -> 20.0");
  }
  {
    // Levels only step down once the output is level_deadband past the boundary.
    StartController c = make();
    c.update(input(0, 0.1f));
    StartDecision d = c.update(input(2 * S, -0.04f));
    check(d.level == Level::START && d.state == HeadState::START, "deadband: a dip of 0.04 keeps the start");
    d = c.update(input(4 * S, -0.06f));
    check(d.level == Level::KEEP && d.state == HeadState::IDLE && d.transition == Transition::STOPPED_PID,
          "deadband: past 0.05 the start is dropped (the unit never ran)");
    d = c.update(input(6 * S, 0.0f));
    check(d.state == HeadState::START, "a start the unit never ran doesn't count as a stop: starts again at once");
  }
  {
    // A run: the PID's output drives the setpoint across the levels.
    StartController c = running(0, 0.3f);
    StartInput in = input(30 * S, 0.3f);
    in.operating = true;
    in = at_room(in, 20.08f, 0.3f);
    StartDecision d = c.update(in);
    check(d.state == HeadState::RUN && near(d.setpoint, 20.5f), "run: start level while the output is past the room");
    in.now_ms = MIN;
    in = at_room(in, 20.2f, -0.2f);
    d = c.update(in);
    check(d.level == Level::KEEP && near(d.setpoint, 19.5f), "run: running level once the output is below the room");
    in.now_ms = 90 * S;
    in = at_room(in, 20.4f, -0.6f);
    d = c.update(in);
    check(d.level == Level::OFF && d.state == HeadState::RUN && near(d.setpoint, 19.5f),
          "run: off level asked within min_run; running level held");
    in.now_ms = 2 * MIN + 10 * S;
    in.device_view = 20.5f;
    d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_PID, "run: stopped by the PID after min_run");
    check(near(d.setpoint, 19.5f), "stopped: off level, two steps below the reported 20.5");
    in.now_ms = 2 * MIN + 20 * S;  // the unit still reports running for a few seconds
    d = c.update(in);
    check(d.state == HeadState::IDLE, "a lagging 'running' report after a stop is not an unrequested start");
    in.operating = false;
    in = at_room(in, 20.3f, -0.3f);
    in.now_ms = 2 * MIN + 40 * S;
    d = c.update(in);
    check(near(d.setpoint, 19.5f), "off level within stop_grace of a commanded stop, though the PID asks to keep");
    in.now_ms = 3 * MIN + 20 * S;
    d = c.update(in);
    check(near(d.setpoint, 20.0f), "running level once stop_grace has passed");
    in.now_ms = 3 * MIN + 30 * S;
    in.operating = true;  // the unit restarted on its own at the running level
    d = c.update(in);
    check(d.state == HeadState::RUN && d.transition == Transition::UNREQUESTED_START, "a restart at the running level is tracked as a run");
  }
  {
    // The unit stops itself; the next start waits out the minimum cycle.
    StartController c = running(0, 0.3f);
    StartInput in = input(3 * MIN, 0.3f);
    StartDecision d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_BY_UNIT, "run: the unit stopped by itself");
    in.now_ms = 3 * MIN + 30 * S;
    d = c.update(in);
    check(d.state == HeadState::IDLE && near(d.setpoint, 19.5f), "no start within the minimum cycle: running level");
    in.now_ms = 4 * MIN;
    d = c.update(in);
    check(d.state == HeadState::START, "starts again after the minimum cycle");
  }
  {
    // No start while shared heat arrives or while hysteresis holds.
    StartController c = make();
    StartInput in = input(0, 0.3f);
    in.shared_heat = true;
    StartDecision d = c.update(in);
    check(d.state == HeadState::IDLE && near(d.setpoint, 19.0f), "no start while heat arrives from other heads; off level");
    in.shared_heat = false;
    in.hold = true;
    d = c.update(in);
    check(d.state == HeadState::IDLE && near(d.setpoint, 19.0f), "no start while hysteresis holds; off level");
    in.hold = false;
    check(c.update(in).state == HeadState::START, "starts once nothing holds it back");
  }
  {
    // Shared heat stops a run, and ends a start.
    StartController c = running(0, 0.3f);
    StartInput in = input(30 * S, 0.3f);
    in.operating = true;
    in.shared_heat = true;
    StartDecision d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_SHARED, "shared heat stops a run");
    StartController s = make();
    s.update(input(0, 0.3f));
    in = input(2 * MIN, 0.3f);
    in.shared_heat = true;
    d = s.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_SHARED && near(d.setpoint, 19.0f),
          "shared heat ends a start");
  }
  {
    // A start is held while the PID asks for it; a long one is reported once.
    StartController c = make();
    c.update(input(0, 0.3f));
    StartDecision d = c.update(input(10 * MIN, 0.3f));
    check(d.state == HeadState::START && near(d.setpoint, 20.0f), "start held at 10 min (no timeout)");
    d = c.update(input(15 * MIN, 0.3f));
    check(d.state == HeadState::START && d.transition == Transition::START_FAULT, "fault reported at 15 min");
    d = c.update(input(15 * MIN + 2 * S, 0.3f));
    check(d.state == HeadState::START && d.transition == Transition::NONE, "fault reported once; start kept");
  }
  {
    // Hailee's room 2026-11-19 01:44: switched on straight into a start, the
    // unit reported running for 20 s and stopped. That is not a run: the start
    // level stays until running holds for run_confirm_ms.
    StartController c = make_confirming();
    StartDecision d = c.update(input(0, 0.3f));
    check(d.state == HeadState::START, "confirm: starts");
    StartInput in = input(10 * S, 0.3f);
    in.operating = true;
    d = c.update(in);
    check(d.state == HeadState::START && near(d.setpoint, 20.0f), "confirm: running, not yet a run; start level kept");
    in.now_ms = 30 * S;
    in.operating = false;
    d = c.update(in);
    check(d.state == HeadState::START && d.transition == Transition::NONE && near(d.setpoint, 20.0f),
          "confirm: a 20 s spell is not a run; still asking the unit to start");
    in.now_ms = 90 * S;
    in.operating = true;
    d = c.update(in);
    check(d.state == HeadState::START, "confirm: running again, the confirmation restarts");
    in.now_ms = 140 * S;
    d = c.update(in);
    check(d.state == HeadState::START, "confirm: not before run_confirm_ms of running");
    in.now_ms = 150 * S;
    d = c.update(in);
    check(d.state == HeadState::RUN && d.transition == Transition::RUNNING, "confirm: a run after 60 s of running");
  }
  {
    // Riding the primary bedroom: Hailee's room 0.28 short of target wants a
    // start, but the primary is forecast to start in 2 min. Wait for it.
    StartController c = make();
    StartInput in = input(0, 0.3f);
    in.shared_expected_s = 120.0f;
    StartDecision d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::DEFERRED, "defer: start held off for expected heat");
    check(near(d.setpoint, 19.0f), "defer: off level");
    in.now_ms = 60 * S;
    d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::NONE, "defer: logged once");
  }
  {
    // A deferral ends at the floor, at defer_max, and when the forecast goes.
    StartController c = make();
    StartInput in = input(0, 0.3f);
    in.shared_expected_s = 60.0f;
    c.update(in);
    in.now_ms = 2 * MIN;
    in = at_room(in, 19.75f, 0.3f);  // 0.53 short: past the 0.5 floor
    check(c.update(in).state == HeadState::START, "defer: starts anyway past the floor");

    StartController m = make();
    in = input(0, 0.3f);
    in.shared_expected_s = 0.0f;  // forecast overdue, the primary never starts
    m.update(in);
    in.now_ms = 6 * MIN - S;
    check(m.update(in).state == HeadState::IDLE, "defer: still waiting before defer_max");
    in.now_ms = 6 * MIN;
    check(m.update(in).state == HeadState::START, "defer: starts anyway at defer_max");

    StartController g = make();
    in = input(0, 0.3f);
    in.shared_expected_s = 200.0f;
    g.update(in);
    in.now_ms = 60 * S;
    in.shared_expected_s = NAN;
    check(g.update(in).state == HeadState::START, "defer: starts once the forecast is dropped");

    StartController far = make();
    in = input(0, 0.3f);
    in.shared_expected_s = 400.0f;  // beyond the 5 min window
    check(far.update(in).state == HeadState::START, "defer: a forecast beyond the window doesn't defer");
  }
  {
    // A parked head below target switches on to take expected heat, before
    // its own on_threshold; not when at target.
    StartController c = make();
    StartInput in = at_room(input(0, 0.0f), 20.15f, 0.0f);  // 0.13 short: inside the band (on at 0.25)
    in.powered = false;
    StartDecision d = c.update(in);
    check(d.state == HeadState::PARKED && d.power == PowerRequest::NONE, "catch: parked inside the band");
    in.now_ms = 2 * MIN;
    in.shared_expected_s = 90.0f;
    d = c.update(in);
    check(d.power == PowerRequest::TURN_ON && d.transition == Transition::CATCH, "catch: switched on for expected heat");
    StartController a = make();
    in = at_room(input(0, 0.0f), 20.25f, 0.0f);  // 0.03 short: close enough
    in.powered = false;
    a.update(in);
    in.now_ms = 2 * MIN;
    in.shared_heat = true;
    check(a.update(in).power == PowerRequest::NONE, "catch: not when at target");
  }
  {
    // A run stops after min_run once the primary is forecast within 2 min.
    StartController c = running(0, 0.3f);
    StartInput in = input(60 * S, 0.3f);
    in.operating = true;
    in.shared_expected_s = 60.0f;
    check(c.update(in).state == HeadState::RUN, "expected stop: not before the minimum run");
    in.now_ms = 2 * MIN + 10 * S;
    in.shared_expected_s = 180.0f;
    check(c.update(in).state == HeadState::RUN, "expected stop: a forecast beyond 2 min doesn't stop it");
    in.now_ms = 2 * MIN + 20 * S;
    in.shared_expected_s = 100.0f;
    StartDecision d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_EXPECTED && near(d.setpoint, 19.0f),
          "expected stop: stopped for expected heat, off level");
  }
  {
    // Past the defer_floor short of target, a run is not stopped for expected heat.
    StartController c = make();
    StartInput in = at_room(input(0, 0.3f), 19.75f, 0.3f);  // 0.53 short
    c.update(in);
    in.now_ms = 10 * S;
    in.operating = true;
    c.update(in);
    in.now_ms = 2 * MIN + 20 * S;
    in.shared_expected_s = 0.0f;
    StartDecision d = c.update(in);
    check(d.state == HeadState::RUN && d.transition == Transition::NONE,
          "expected stop: not past the floor (0.53 short)");
  }
  {
    // A forecast that never comes true (the main floor parked, flat at its
    // start level, "starting now" for an hour): the head gives way once, then
    // runs to the PID's stop rather than being cut at every min_run.
    StartController c = running(0, 0.3f);
    StartInput in = input(2 * MIN + 20 * S, 0.3f);
    in.operating = true;
    in.shared_expected_s = 0.0f;
    StartDecision d = c.update(in);
    check(d.transition == Transition::STOPPED_EXPECTED, "overdue forecast: the first run is stopped for it");
    in.now_ms = 2 * MIN + 30 * S;
    in.operating = false;
    c.update(in);
    in.now_ms = 3 * MIN + 30 * S;  // past min_cycle and stop_grace
    d = c.update(in);
    check(d.state == HeadState::START && d.transition == Transition::STARTED,
          "overdue forecast: the next start is not deferred again");
    in.now_ms = 3 * MIN + 40 * S;
    in.operating = true;
    c.update(in);
    in.now_ms = 6 * MIN;  // past min_run
    d = c.update(in);
    check(d.state == HeadState::RUN && d.transition == Transition::NONE,
          "overdue forecast: the next run is not stopped for it");
    check(near(d.setpoint, 20.0f), "overdue forecast: the run keeps the start level");
    // Cleared and forecast anew: the head gives way again.
    in.now_ms = 6 * MIN + 10 * S;
    in.shared_expected_s = NAN;
    c.update(in);
    in.now_ms = 6 * MIN + 20 * S;
    in.shared_expected_s = 60.0f;
    check(c.update(in).transition == Transition::STOPPED_EXPECTED, "a new forecast is owed a stop again");
  }
  {
    // A deferral that ran out counts as giving way: the run it ends in is not stopped for the same forecast.
    StartController m = make();
    StartInput in = input(0, 0.3f);
    in.shared_expected_s = 0.0f;
    m.update(in);
    in.now_ms = 6 * MIN;
    check(m.update(in).state == HeadState::START, "deferral ran out: starts");
    in.now_ms = 6 * MIN + 10 * S;
    in.operating = true;
    m.update(in);
    in.now_ms = 8 * MIN + 30 * S;
    StartDecision d = m.update(in);
    check(d.state == HeadState::RUN && d.transition == Transition::NONE,
          "deferral ran out: the run is not stopped for the same forecast");
  }
  {
    // While idle after giving way, the running level is sent again (the unit
    // may restart on its own), not the off level that waiting asks for.
    StartController c = running(0, -0.2f);  // the PID asks to keep running
    StartInput in = input(2 * MIN + 20 * S, -0.2f);
    in.operating = true;
    in.shared_expected_s = 0.0f;
    check(c.update(in).transition == Transition::STOPPED_EXPECTED, "idle after giving way: stopped once");
    in.now_ms = 4 * MIN;  // past stop_grace
    in.operating = false;
    check(near(c.update(in).setpoint, 19.5f), "idle after giving way: running level, not the off level");
  }
  {
    // Park and unpark on the hysteresis band.
    StartController c = make();
    StartInput in = at_room(input(0, 0.0f), 20.6f, -0.6f);  // +0.32: inside the band
    in.shared_heat = true;
    StartDecision d = c.update(in);
    check(d.power == PowerRequest::NONE && d.state == HeadState::IDLE,
          "shared heat inside the band: stays powered and takes the leak");
    in.now_ms = 2 * S;
    in = at_room(in, 20.7f, -0.6f);  // +0.42: past off_threshold
    d = c.update(in);
    check(d.power == PowerRequest::TURN_OFF && d.transition == Transition::PARK, "park: shared heat past off_threshold");
    in.now_ms = 5 * S;
    in.powered = false;
    d = c.update(in);
    check(d.state == HeadState::PARKED && d.power == PowerRequest::NONE, "parked once the device is off");
    check(d.freeze_integral, "parked: integral frozen");
    check(near(d.setpoint, 19.0f), "parked: the setpoint to switch on with is the off level");
    in.now_ms = 2 * MIN;
    in.shared_heat = false;
    in = at_room(in, 20.1f, 0.3f);  // -0.18, the PID asks for a start
    d = c.update(in);
    check(d.power == PowerRequest::NONE, "parked: the PID alone does not unpark inside the band");
    in = at_room(in, 20.0f, 0.3f);  // -0.28: past on_threshold
    d = c.update(in);
    check(d.power == PowerRequest::TURN_ON && d.transition == Transition::UNPARK, "unpark: room past on_threshold");
    in.now_ms = 2 * MIN + 5 * S;
    in.powered = true;
    in = at_room(in, 20.0f, -0.6f);
    d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::UNPARKED, "unparked: idle again");
    // Shared heat past the band again: may not park within the minimum cycle.
    in.shared_heat = true;
    in = at_room(in, 20.7f, -0.6f);
    d = c.update(in);
    check(d.power == PowerRequest::NONE, "no park again within the minimum cycle");
    in.now_ms = 4 * MIN;
    d = c.update(in);
    check(d.power == PowerRequest::TURN_OFF, "parks again after the minimum cycle");
  }
  {
    // Without shared heat the controller leaves the cut to hysteresis (and its off_delay).
    StartController c = make();
    check(c.update(at_room(input(0, 0.0f), 20.8f, -0.8f)).power == PowerRequest::NONE,
          "no park past the band without shared heat");
  }
  {
    // A hysteresis hold (past off_threshold, waiting out off_delay) stops a run
    // and abandons a start.
    StartController c = running(0, 0.3f);
    StartInput in = input(30 * S, 0.3f);
    in.operating = true;
    in.hold = true;
    StartDecision d = c.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_HOLD, "hold: run stopped before the minimum run");
    check(near(d.setpoint, 19.0f), "hold: off level stops the compressor");
    StartController s = make();
    s.update(input(0, 0.3f));
    in = input(2 * S, 0.3f);
    in.hold = true;
    d = s.update(in);
    check(d.state == HeadState::IDLE && d.transition == Transition::STOPPED_HOLD, "hold: start abandoned");
  }
  {
    // The integral is frozen only while parked.
    StartController c = running(0, 0.3f);
    StartInput in = input(30 * S, 0.3f);
    in.operating = true;
    check(!c.update(in).freeze_integral, "run: integral accumulates");
  }
  {
    // Cooling mirrors heating: the start level is the step at or below the room.
    StartController c = make();
    StartInput in = input(0, 0.0f);
    in.heating = false;
    in.device_view = 24.0f;
    in.target = 23.5f;
    in.room = 24.08f;
    in.output = 24.08f - 0.3f;  // the PID asks for a setpoint 0.3 below the room
    StartDecision d = c.update(in);
    check(d.state == HeadState::START && near(d.setpoint, 24.0f), "cooling: start level 24.0 for room 24.08");
    in.now_ms = 10 * S;
    in.operating = true;
    in.output = 24.08f + 0.2f;
    d = c.update(in);
    check(d.level == Level::KEEP && near(d.setpoint, 24.5f), "cooling: running level one step above the reported");
    in.now_ms = 3 * MIN;
    in.output = 24.08f + 0.6f;
    d = c.update(in);
    check(d.transition == Transition::STOPPED_PID && near(d.setpoint, 25.0f), "cooling: PID stop, off level two steps above");
  }
  {
    // Cooling: parked once the room is off_threshold below the target.
    StartController c = make();
    StartInput in = input(0, 0.0f);
    in.heating = false;
    in.device_view = 23.0f;
    in.room = 23.0f;
    in.output = 23.6f;
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
    // Without the unit's report, the room rounded to the step stands in; an
    // unknown output asks for nothing.
    StartController c = make();
    StartInput in = at_room(input(0, 0.0f), 20.3f, -0.6f);
    in.device_view = NAN;
    check(near(c.update(in).setpoint, 19.5f), "no reported temperature: room rounded to the step");
    in.output = NAN;
    check(c.update(in).level == Level::OFF, "no PID output: off level");
  }

  {
    // resync (continuous mode): the state is adopted from the device.
    StartController c = make();
    StartInput in = input(10 * MIN, -0.6f);
    in.powered = false;
    c.resync(in, false, 0, false, 0);
    check(c.state() == HeadState::PARKED, "resync: device off -> PARKED");
    in.powered = true;
    in.operating = true;
    c.resync(in, false, 0, false, 0);
    check(c.state() == HeadState::RUN, "resync: running -> RUN");
    in.operating = false;
    c.resync(in, false, 0, false, 0);
    check(c.state() == HeadState::IDLE, "resync: powered, idle -> IDLE");
  }
  {
    // resync: a run in progress is stopped once heat arrives from other heads.
    StartController c = make();
    StartInput in = input(10 * MIN, 0.2f);
    in.operating = true;
    c.resync(in, false, 0, false, 0);
    in.shared_heat = true;
    StartDecision d = c.update(in);
    check(d.transition == Transition::STOPPED_SHARED && c.state() == HeadState::IDLE,
          "resync: an adopted run stops on shared heat");
    check(near(d.setpoint, 19.0f), "resync: off level after the stop");
  }
  {
    // resync: a power change seen while not engaged still holds the minimum cycle.
    StartController c = make();
    StartInput in = at_room(input(10 * MIN, -0.6f), 20.8f, -0.6f);
    in.shared_heat = true;
    c.resync(in, true, 10 * MIN - 30 * S, false, 0);
    check(c.update(in).power == PowerRequest::NONE, "resync: no park within min_cycle of a power change");
    in.now_ms = 10 * MIN + 31 * S;
    check(c.update(in).power == PowerRequest::TURN_OFF, "resync: parks once min_cycle has passed");
  }
  {
    // resync: a stop seen while not engaged holds the next start.
    StartController c = make();
    StartInput in = input(10 * MIN, 0.2f);
    c.resync(in, false, 0, true, 10 * MIN - 30 * S);
    check(c.update(in).transition == Transition::NONE, "resync: no start within min_cycle of a stop");
    in.now_ms = 10 * MIN + 31 * S;
    check(c.update(in).transition == Transition::STARTED, "resync: starts once min_cycle has passed");
  }
  {
    // resync: a parked head catches heat from other heads without a fresh init.
    StartController c = make();
    StartInput in = at_room(input(10 * MIN, 0.3f), 20.1f, 0.3f);
    in.powered = false;
    in.shared_heat = true;
    c.resync(in, false, 0, false, 0);
    check(c.update(in).power == PowerRequest::TURN_ON, "resync: parked and short of target -> catch");
  }

  std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
