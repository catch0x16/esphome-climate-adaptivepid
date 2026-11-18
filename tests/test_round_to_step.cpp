// Native test of setpoint rounding to the device step. No ESPHome needed:
//   c++ -std=c++17 -Wall -Wextra -Icomponents/climate_adaptivepid -o /tmp/test_round_to_step \
//     tests/test_round_to_step.cpp && /tmp/test_round_to_step
#include <cmath>
#include <cstdio>

#include "floats.h"

using devicestate::round_to_step;
using devicestate::StepRounding;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static bool near(float a, float b) { return std::fabs(a - b) <= 0.0001f; }

int main() {
  // 67°F target, as seen on a 0.5°C device.
  const float f67 = (67.0f - 32.0f) * 5.0f / 9.0f;  // 19.444
  check(near(round_to_step(f67, 0.5f, StepRounding::Nearest), 19.5f), "nearest: 19.44 rounds up to 19.5");
  check(near(round_to_step(f67, 0.5f, StepRounding::Down), 19.0f), "heating: 19.44 rounds down to 19.0");
  check(near(round_to_step(f67, 0.5f, StepRounding::Up), 19.5f), "cooling: 19.44 rounds up to 19.5");

  // A PID backing off just past a grid line is not pushed back up.
  check(near(round_to_step(20.78f, 0.5f, StepRounding::Down), 20.5f), "heating: 20.78 rounds down to 20.5");
  check(near(round_to_step(21.22f, 0.5f, StepRounding::Up), 21.5f), "cooling: 21.22 rounds up to 21.5");

  // Values on the grid, including float noise either side, stay put.
  check(near(round_to_step(20.5f, 0.5f, StepRounding::Down), 20.5f), "heating: 20.5 stays 20.5");
  check(near(round_to_step(20.4999f, 0.5f, StepRounding::Down), 20.5f), "heating: 20.4999 is 20.5, not 20.0");
  check(near(round_to_step(20.5001f, 0.5f, StepRounding::Up), 20.5f), "cooling: 20.5001 is 20.5, not 21.0");
  const float sum = 19.1f + 0.4f;  // float arithmetic landing near 19.5
  check(near(round_to_step(sum, 0.5f, StepRounding::Down), 19.5f), "heating: 19.1 + 0.4 is 19.5");
  check(near(round_to_step(sum, 0.5f, StepRounding::Up), 19.5f), "cooling: 19.1 + 0.4 is 19.5");

  // Other steps.
  check(near(round_to_step(21.37f, 1.0f, StepRounding::Down), 21.0f), "1.0 step: 21.37 down is 21");
  check(near(round_to_step(21.37f, 1.0f, StepRounding::Up), 22.0f), "1.0 step: 21.37 up is 22");
  check(near(round_to_step(21.37f, 0.1f, StepRounding::Nearest), 21.4f), "0.1 step: 21.37 nearest is 21.4");

  // No step: the value passes through.
  check(near(round_to_step(21.37f, 0.0f, StepRounding::Down), 21.37f), "no step: unchanged");

  // Deadband against the held setpoint.
  using devicestate::round_to_step_held;
  const float m = 0.1f;
  // Heating, held at 21.0: stepping up needs 21.5 + margin.
  check(near(round_to_step_held(21.52f, 0.5f, StepRounding::Down, 21.0f, m), 21.0f),
        "heating: 21.52 just past the boundary holds 21.0");
  check(near(round_to_step_held(21.59f, 0.5f, StepRounding::Down, 21.0f, m), 21.0f),
        "heating: 21.59 still inside the margin holds 21.0");
  check(near(round_to_step_held(21.61f, 0.5f, StepRounding::Down, 21.0f, m), 21.5f),
        "heating: 21.61 past the margin steps up to 21.5");
  check(near(round_to_step_held(23.20f, 0.5f, StepRounding::Down, 21.0f, m), 23.0f),
        "heating: a large step up goes as far as the margin allows");
  check(near(round_to_step_held(22.55f, 0.5f, StepRounding::Down, 21.0f, m), 22.0f),
        "heating: each boundary needs the margin, 22.55 reaches 22.0");
  // Heating, held at 21.5: stepping down is immediate.
  check(near(round_to_step_held(21.49f, 0.5f, StepRounding::Down, 21.5f, m), 21.0f),
        "heating: 21.49 drops to 21.0 at once");
  check(near(round_to_step_held(21.52f, 0.5f, StepRounding::Down, 21.5f, m), 21.5f),
        "heating: 21.52 keeps 21.5");
  // A value flipping across a boundary does not flip the device.
  {
    float held = 21.0f;
    int changes = 0;
    const float samples[] = {21.48f, 21.53f, 21.47f, 21.55f, 21.51f, 21.49f, 21.56f};
    for (float v : samples) {
      const float next = round_to_step_held(v, 0.5f, StepRounding::Down, held, m);
      if (!near(next, held)) changes++;
      held = next;
    }
    check(changes == 0, "heating: jitter of +-0.05 at a boundary sends no change");
  }
  // Cooling mirrors: held at 24.0, stepping down (more cooling) needs 23.5 - margin.
  check(near(round_to_step_held(23.45f, 0.5f, StepRounding::Up, 24.0f, m), 24.0f),
        "cooling: 23.45 inside the margin holds 24.0");
  check(near(round_to_step_held(23.39f, 0.5f, StepRounding::Up, 24.0f, m), 23.5f),
        "cooling: 23.39 past the margin steps down to 23.5");
  check(near(round_to_step_held(23.51f, 0.5f, StepRounding::Up, 23.5f, m), 24.0f),
        "cooling: 23.51 rises to 24.0 at once");
  // No deadband where none applies.
  check(near(round_to_step_held(21.52f, 0.5f, StepRounding::Down, NAN, m), 21.5f), "no held setpoint: plain floor");
  check(near(round_to_step_held(21.52f, 0.5f, StepRounding::Down, 21.0f, 0.0f), 21.5f), "no margin: plain floor");
  check(near(round_to_step_held(21.30f, 0.5f, StepRounding::Nearest, 21.0f, m), 21.5f), "nearest: no deadband");

  std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
