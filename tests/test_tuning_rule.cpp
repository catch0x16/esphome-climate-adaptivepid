// Native test of the tuning rule:
//   c++ -std=c++17 -Wall -Wextra -Icomponents/workflow_climate -o /tmp/test_tuning_rule \
//     tests/test_tuning_rule.cpp && /tmp/test_tuning_rule
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "tuning_rule.h"

using workflow::pid::Gains;
using workflow::pid::TuningConfig;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

static const float PER_HOUR = 1.0f / 3600.0f;

int main() {
  TuningConfig config;

  // 3 C/h with the defaults (15 minute time scale, 0.75 gap) gives kp 1.0.
  const Gains gains = workflow::pid::gains_from_rate(3.0f * PER_HOUR, config);
  check(near(gains.kp, 1.0f, 0.001f), "rule: 3 C/h gives kp 1.0");
  check(near(gains.ki, 1.0f / 3600.0f, 0.00001f), "rule: ki = kp / (4 * 15 min)");
  check(near(workflow::pid::rate_from_kp(gains.kp, config), 3.0f * PER_HOUR, 0.00001f),
        "rule: kp maps back to the rate");
  check(near(workflow::pid::gains_from_rate(100.0f * PER_HOUR, config).kp, config.kp_min, 0.001f),
        "rule: kp lower bound");
  check(near(workflow::pid::gains_from_rate(0.01f * PER_HOUR, config).kp, config.kp_max, 0.001f),
        "rule: kp upper bound");

  // A longer learned dead time gives a gentler loop: 10 + 10 minutes halves kp.
  const Gains slow = workflow::pid::gains_from_rate(3.0f * PER_HOUR, 600.0f, config);
  check(near(slow.kp, 0.75f, 0.001f), "rule: the learned dead time enters the time scale");
  check(near(slow.ki, 0.75f / (4.0f * 1200.0f), 0.000001f), "rule: and ki follows it");

  check(workflow::pid::clamp_rate(NAN) == workflow::pid::MIN_RATE, "rates: NaN is clamped to the minimum");
  check(workflow::pid::clamp_rate(1.0f) == workflow::pid::MAX_RATE, "rates: clamped to the maximum");

  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all passed\n");
  return EXIT_SUCCESS;
}
