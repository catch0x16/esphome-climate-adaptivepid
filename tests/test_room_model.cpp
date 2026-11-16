// Native test of the online room model against a simulated room:
//   c++ -std=c++17 -Wall -Wextra -Icomponents/workflow_climate -o /tmp/test_room_model \
//     tests/test_room_model.cpp && /tmp/test_room_model
//
// The simulated room follows the model's own equation with known values, a
// transport delay between the device's output and the room (the dead time),
// and a sensor that reads in 0.1 C steps.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "room_model.h"

using workflow::pid::ModelSample;
using workflow::pid::ModelState;
using workflow::pid::RoomModel;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

static const float PER_HOUR = 1.0f / 3600.0f;
static const uint32_t STEP_MS = 10000;

struct Room {
  // Per hour.
  float loss{0.1f};
  float heat_rate{3.0f};
  float cool_rate{2.5f};
  float delay_minutes{6.0f};
  float temperature{20.0f};
  std::vector<float> pipe;  // output on its way to the room
  size_t head{0};

  void reset_pipe() {
    pipe.assign(static_cast<size_t>(delay_minutes * 60000.0f / STEP_MS), 0.0f);
    head = 0;
  }

  // One step: `output` leaves the device now and reaches the room after the delay.
  void step(float output, bool heating, float outdoor, float extra_per_hour) {
    float delivered = output;
    if (!pipe.empty()) {
      delivered = pipe[head];
      pipe[head] = output;
      head = (head + 1) % pipe.size();
    }
    const float drive = heating ? heat_rate * delivered : -cool_rate * delivered;
    const float rate = drive - loss * (temperature - outdoor) + extra_per_hour;
    temperature += rate * (STEP_MS / 1000.0f) / 3600.0f;
  }

  float sensor() const { return std::round(temperature * 10.0f) / 10.0f; }
};

struct Sim {
  RoomModel model;
  Room room;
  uint32_t now{1000};
  float outdoor{5.0f};
  bool outdoor_known{true};

  Sim() { room.reset_pipe(); }

  // Run for `minutes`. The device reports `operating`, `output` and
  // `defrosting`; the room receives `delivered` (normally the same) plus
  // `extra` C/h (sun, an open window, a defrost...).
  void run(float minutes, bool operating, bool heating, float output, float delivered, float extra = 0.0f,
           bool defrosting = false) {
    const int steps = static_cast<int>(minutes * 60000.0f / STEP_MS);
    for (int i = 0; i < steps; i++) {
      now += STEP_MS;
      room.step(operating ? delivered : 0.0f, heating, outdoor, extra);
      model.update(ModelSample{now, room.sensor(), outdoor_known ? outdoor : NAN, heating, operating,
                               operating ? output : 0.0f, defrosting});
    }
  }
  void run(float minutes, bool operating, bool heating, float output) {
    run(minutes, operating, heating, output, output);
  }

  // On/off heating between `low` and `high` at `output`.
  void heat_cycles(int cycles, float low, float high, float output) {
    for (int c = 0; c < cycles; c++) {
      for (int i = 0; i < 600 && room.sensor() > low; i++) run(1, false, true, 0.0f);
      for (int i = 0; i < 600 && room.sensor() < high; i++) run(1, true, true, output);
    }
  }
  void cool_cycles(int cycles, float low, float high, float output) {
    for (int c = 0; c < cycles; c++) {
      for (int i = 0; i < 600 && room.sensor() < high; i++) run(1, false, false, 0.0f);
      for (int i = 0; i < 600 && room.sensor() > low; i++) run(1, true, false, output);
    }
  }
};

int main() {
  {
    // Output estimate from the setpoint the device was given.
    using workflow::pid::estimate_output;
    check(estimate_output(true, true, 22.0f, 21.0f, 0.75f) == 1.0f, "output: full past full_output_gap");
    check(near(estimate_output(true, true, 21.5f, 21.2f, 0.75f), 0.4f, 0.001f), "output: proportional below it");
    check(estimate_output(true, true, 21.0f, 21.2f, 0.75f) == 0.0f, "output: setpoint below the room gives none");
    check(near(estimate_output(true, false, 23.5f, 24.0f, 0.75f), 2.0f / 3.0f, 0.001f), "output: cooling mirrors it");
    check(estimate_output(false, true, 25.0f, 20.0f, 0.75f) == 0.0f, "output: none while not operating");
    check(estimate_output(true, true, NAN, 20.0f, 0.75f) == 0.0f, "output: none without a setpoint");
  }
  {
    // Heating cycles at full output: loss, heating rate and dead time converge
    // on the room's (0.1 /h, 3 C/h, 6 min).
    Sim sim;
    sim.heat_cycles(80, 20.0f, 21.0f, 1.0f);
    const ModelState &m = sim.model.state();
    std::printf("      heating cycles: loss %.3f /h (%u), rate %.2f C/h (%u), dead %.1f min (%u)\n",
                m.loss * 3600.0f, sim.model.loss_measurements(), m.heat_rate * 3600.0f,
                sim.model.rate_measurements(true), m.heat_dead_s / 60.0f, sim.model.dead_time_measurements(true));
    check(near(m.loss * 3600.0f, 0.1f, 0.03f), "cycles: heat loss within 30%");
    check(near(m.heat_rate * 3600.0f, 3.0f, 0.45f), "cycles: heating rate within 15%");
    check(near(m.heat_dead_s / 60.0f, 6.0f, 2.5f), "cycles: dead time within 2.5 minutes");
    check(near(m.cool_rate, workflow::pid::DEFAULT_RATE, 1e-9f), "cycles: cooling rate untouched by heating");
  }
  {
    // The regression this model fixes: a room held steady at low output used
    // to read as a slow room and drive kp to its maximum.
    Sim sim;
    sim.heat_cycles(60, 20.0f, 21.0f, 1.0f);
    const float learned = sim.model.rate(true);
    const uint32_t before = sim.model.rate_measurements(true);
    // Equilibrium at 0.25 output: 3 C/h * 0.25 = 0.75 C/h = loss 0.1/h * 7.5 C.
    sim.room.temperature = 12.5f;
    sim.run(30 * 60, true, true, 0.25f);
    check(sim.model.rate_measurements(true) == before, "holding: low output teaches nothing about the rate");
    check(sim.model.rate(true) == learned, "holding: the rate is unchanged after 30 hours");
  }
  {
    // Held steady at half output: the rate is still found, since the loss
    // explains why the room does not move.
    Sim sim;
    sim.heat_cycles(60, 20.0f, 21.0f, 1.0f);  // learn the loss first
    ModelState state = sim.model.state();
    state.heat_rate = 1.0f * PER_HOUR;  // start far off
    sim.model.set_state(state);
    // Equilibrium at 0.5 output: 1.5 C/h = 0.1/h * 15 C.
    sim.room.temperature = 20.0f;
    sim.run(20 * 60, true, true, 0.5f);
    std::printf("      held at half output: rate %.2f C/h\n", sim.model.rate(true) * 3600.0f);
    check(near(sim.model.rate(true) * 3600.0f, 3.0f, 0.45f), "half output: the rate is found while holding");
  }
  {
    // Partial-output runs agree with full-output runs when the output estimate is right.
    Sim sim;
    sim.heat_cycles(80, 20.0f, 21.0f, 0.6f);
    std::printf("      cycles at 0.6 output: rate %.2f C/h\n", sim.model.rate(true) * 3600.0f);
    check(near(sim.model.rate(true) * 3600.0f, 3.0f, 0.45f), "partial output: normalised to full output");
  }
  {
    // Defrost, reported by the device: the room gets no heat and loses some.
    // No window is taken during one or within a dead time after it. Harsher
    // than real units: 20 minutes in every 50.
    Sim sim;
    sim.heat_cycles(60, 20.0f, 21.0f, 1.0f);
    const float learned = sim.model.rate(true);
    for (int i = 0; i < 20; i++) {
      sim.run(30, true, true, 1.0f, 1.0f);
      sim.run(20, true, true, 1.0f, 0.0f, -3.0f, true);
    }
    std::printf("      with reported defrosts: rate %.2f C/h\n", sim.model.rate(true) * 3600.0f);
    check(sim.model.defrost_samples() > 0, "defrost: reported defrost samples are seen");
    check(near(sim.model.rate(true), learned, 0.2f * PER_HOUR), "defrost: the rate stays within 0.2 C/h");
  }
  {
    // A disturbance the device cannot report (a window opened for 15 minutes
    // in every 75 of heating): wrong-way stretches are dropped as anomalies.
    Sim sim;
    sim.heat_cycles(60, 20.0f, 21.0f, 1.0f);
    const float learned = sim.model.rate(true);
    for (int i = 0; i < 20; i++) {
      sim.run(60, true, true, 1.0f, 1.0f);
      sim.run(15, true, true, 1.0f, 1.0f, -6.0f);
    }
    std::printf("      with an open window: rate %.2f C/h, %u windows dropped\n", sim.model.rate(true) * 3600.0f,
                sim.model.anomalies());
    check(sim.model.anomalies() > 0, "disturbance: windows are dropped as anomalies");
    check(near(sim.model.rate(true), learned, 0.5f * PER_HOUR), "disturbance: the rate stays within 0.5 C/h");
  }
  {
    // Cooling, outdoor above the room.
    Sim sim;
    sim.outdoor = 32.0f;
    sim.room.temperature = 25.0f;
    sim.cool_cycles(80, 24.0f, 25.0f, 1.0f);
    const ModelState &m = sim.model.state();
    std::printf("      cooling cycles: loss %.3f /h, rate %.2f C/h, dead %.1f min\n", m.loss * 3600.0f,
                m.cool_rate * 3600.0f, m.cool_dead_s / 60.0f);
    check(near(m.loss * 3600.0f, 0.1f, 0.03f), "cooling: heat loss within 30%");
    check(near(m.cool_rate * 3600.0f, 2.5f, 0.375f), "cooling: cooling rate within 15%");
    check(near(m.cool_dead_s / 60.0f, 6.0f, 2.5f), "cooling: dead time within 2.5 minutes");
  }
  {
    // Without an outdoor temperature the loss is not learned.
    Sim sim;
    sim.outdoor_known = false;
    sim.heat_cycles(40, 20.0f, 21.0f, 1.0f);
    check(sim.model.loss_measurements() == 0, "no outdoor: heat loss is not learned");
    check(sim.model.rate_measurements(true) > 0, "no outdoor: the rate is still learned");
  }
  {
    // A gap in the samples is not joined into a window.
    RoomModel model;
    uint32_t now = 1000;
    float temperature = 20.0f;
    for (int i = 0; i < 6 * 10; i++) {  // 10 minutes running (past the dead time)
      now += STEP_MS;
      model.update(ModelSample{now, temperature, 5.0f, true, true, 1.0f});
    }
    now += 60 * 60 * 1000;  // an hour of nothing
    temperature += 3.0f;
    for (int i = 0; i < 6 * 10; i++) {
      now += STEP_MS;
      model.update(ModelSample{now, temperature, 5.0f, true, true, 1.0f});
    }
    check(model.rate_measurements(true) == 0, "gap: no window spans a gap");
  }

  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all passed\n");
  return EXIT_SUCCESS;
}
