// Native test of the hysteresis off gate. No ESPHome needed:
//   c++ -std=c++17 -Wall -Wextra -Icomponents/workflow_climate -o /tmp/test_off_gate \
//     tests/test_off_gate.cpp && /tmp/test_off_gate
#include <cmath>
#include <cstdio>

#include "off_gate.h"

using workflow::hysterisis::OffAction;
using workflow::hysterisis::OffGate;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static const uint32_t MIN = 60000;

int main() {
  {
    // No delay: today's behaviour, off at the first reading past the threshold.
    OffGate gate(0.75f, NAN, 0);
    check(gate.update(0.70f, 0) == OffAction::NONE, "no delay: below the threshold does nothing");
    check(gate.update(0.80f, 1000) == OffAction::TURN_OFF, "no delay: past the threshold switches off at once");
  }
  {
    // Last night's peaks: past +0.75 for 3 to 4.5 minutes, at most +1.00.
    OffGate gate(0.75f, 1.25f, 5 * MIN);
    check(gate.update(0.80f, 0) == OffAction::HOLD, "delay: past the threshold holds");
    check(gate.update(1.00f, 2 * MIN) == OffAction::HOLD, "delay: still holding at the peak");
    check(gate.held_ms(2 * MIN) == 2 * MIN, "delay: held time counts from the first reading past");
    check(gate.update(0.70f, 4 * MIN + 30000) == OffAction::NONE, "delay: back below after 4.5 min releases the hold");
    check(gate.held_ms(4 * MIN + 30000) == 0, "delay: released, nothing held");
    check(gate.update(0.80f, 10 * MIN) == OffAction::HOLD, "delay: the next excursion starts its own timer");
    check(gate.update(0.80f, 14 * MIN) == OffAction::HOLD, "delay: 4 min into the new excursion still holds");
  }
  {
    OffGate gate(0.75f, 1.25f, 5 * MIN);
    gate.update(0.80f, 0);
    check(gate.update(0.90f, 5 * MIN - 1) == OffAction::HOLD, "delay: just under 5 min holds");
    check(gate.update(0.90f, 5 * MIN) == OffAction::TURN_OFF, "delay: staying past the threshold 5 min switches off");
    check(gate.update(0.90f, 5 * MIN + 2000) == OffAction::TURN_OFF,
          "delay: keeps asking to switch off while the switch-off is throttled");
    gate.reset();
    check(gate.update(0.90f, 6 * MIN) == OffAction::HOLD, "reset: a new excursion starts a new timer");
  }
  {
    // Backstop: a room that really overheats is switched off at once.
    OffGate gate(0.75f, 1.25f, 5 * MIN);
    check(gate.update(0.80f, 0) == OffAction::HOLD, "limit: past the threshold holds");
    check(gate.update(1.30f, 1 * MIN) == OffAction::TURN_OFF, "limit: past the limit switches off at once");
    OffGate fresh(0.75f, 1.25f, 5 * MIN);
    check(fresh.update(1.40f, 0) == OffAction::TURN_OFF, "limit: a first reading past the limit switches off at once");
  }
  {
    OffGate gate(0.75f, 1.25f, 5 * MIN);
    gate.update(0.80f, 0);
    check(gate.update(NAN, 1000) == OffAction::NONE, "no reading: no action, timer cleared");
    check(gate.update(0.80f, 2 * MIN) == OffAction::HOLD, "no reading: timer restarted after it");
    check(gate.update(0.80f, 6 * MIN) == OffAction::HOLD, "no reading: 4 min on the new timer still holds");
  }
  {
    // millis() wraps after ~49 days.
    OffGate gate(0.75f, 1.25f, 5 * MIN);
    const uint32_t start = 0xFFFFFFFFu - MIN;
    gate.update(0.80f, start);
    check(gate.update(0.80f, start + 2 * MIN) == OffAction::HOLD, "wrap: 2 min across the wrap holds");
    check(gate.update(0.80f, start + 5 * MIN) == OffAction::TURN_OFF, "wrap: 5 min across the wrap switches off");
  }

  std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
