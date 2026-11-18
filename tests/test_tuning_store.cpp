// Native test of the boot-time choice between learned and configured response rates:
//   c++ -std=c++17 -Wall -Wextra -Icomponents/climate_adaptivepid -o /tmp/test_tuning_store \
//     tests/test_tuning_store.cpp && /tmp/test_tuning_store
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "tuning_store.h"

using workflow::pid::Resolved;
using workflow::pid::TuningConfig;
using workflow::pid::TuningSource;

static int failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

static const float PER_HOUR = 1.0f / 3600.0f;

// What one mode keeps in flash, as ClimateAdaptivePID stores it.
struct Slot {
  float rate{NAN};
  float seed{NAN};
};

// One boot: resolve against the configured seed, then record the seed in use
// (ClimateAdaptivePID saves at the end of load_tuning_).
static Resolved boot(Slot &slot, float configured) {
  const Resolved resolved = workflow::pid::resolve(slot.rate, slot.seed, configured);
  slot.rate = resolved.value;
  slot.seed = configured;
  return resolved;
}

int main() {
  TuningConfig config;

  {
    // Seeds: an explicit initial rate wins, then kp, then the estimator default.
    check(near(workflow::pid::configured_seed(3.0f * PER_HOUR, 2.0f, config), 3.0f * PER_HOUR, 1e-7f),
          "seed: initial rate is used when configured");
    check(near(workflow::pid::configured_seed(NAN, 1.0f, config), workflow::pid::rate_from_kp(1.0f, config), 1e-7f),
          "seed: kp implies the rate without an initial rate");
    check(workflow::pid::configured_seed(NAN, 0.0f, config) == workflow::pid::DEFAULT_RATE,
          "seed: estimator default without kp or initial rate");
    check(workflow::pid::configured_seed(50.0f * PER_HOUR, 0.0f, config) == workflow::pid::MAX_RATE,
          "seed: clamped to the estimator's range");
    check(workflow::pid::configured_seed(NAN, 1.0f, config) == workflow::pid::configured_seed(NAN, 1.0f, config),
          "seed: deterministic for the same configuration");
  }
  {
    // Same seed keeps what was learned; a changed seed is taken as deliberate.
    const float seed = 2.0f * PER_HOUR;
    Resolved r = workflow::pid::resolve(3.5f * PER_HOUR, seed, seed);
    check(r.source == TuningSource::RESTORED && near(r.value, 3.5f * PER_HOUR, 1e-7f), "resolve: same seed restores");
    r = workflow::pid::resolve(3.5f * PER_HOUR, seed, 4.0f * PER_HOUR);
    check(r.source == TuningSource::SEEDED && near(r.value, 4.0f * PER_HOUR, 1e-7f),
          "resolve: changed seed uses the configuration");
    r = workflow::pid::resolve(NAN, NAN, seed);
    check(r.source == TuningSource::SEEDED && r.value == seed, "resolve: nothing stored uses the configuration");
    r = workflow::pid::resolve(0.0f, seed, seed);
    check(r.source == TuningSource::SEEDED, "resolve: a zero rate is not usable");
    r = workflow::pid::resolve(3.5f * PER_HOUR, NAN, seed);
    check(r.source == TuningSource::SEEDED, "resolve: a missing stored seed uses the configuration");
    r = workflow::pid::resolve(50.0f * PER_HOUR, seed, seed);
    check(r.source == TuningSource::RESTORED && r.value == workflow::pid::MAX_RATE,
          "resolve: a restored rate is clamped");
  }
  {
    // Float noise does not count as a change; a real change does.
    const float seed = workflow::pid::rate_from_kp(0.9f, config);
    check(workflow::pid::same_seed(seed, seed * (1.0f + 1e-6f)), "same_seed: float noise is the same seed");
    check(!workflow::pid::same_seed(seed, workflow::pid::rate_from_kp(0.75f, config)),
          "same_seed: a different kp is a different seed");
    check(!workflow::pid::same_seed(NAN, NAN), "same_seed: NaN never matches");
  }
  {
    // Storage v1 migration: rates learned before seeds were stored are kept,
    // by comparing them against today's seed.
    const float seed = workflow::pid::configured_seed(NAN, 2.0f, config);
    const Resolved r = workflow::pid::resolve(1.8f * PER_HOUR, seed, seed);
    check(r.source == TuningSource::RESTORED && near(r.value, 1.8f * PER_HOUR, 1e-7f),
          "migration: v1 rates are restored");
  }
  {
    // A unit's life across flashes.
    Slot slot;
    const float seed_a = workflow::pid::configured_seed(NAN, 0.9f, config);
    Resolved r = boot(slot, seed_a);
    check(r.source == TuningSource::SEEDED, "lifecycle: first boot starts from the seed");

    slot.rate = 1.2f * PER_HOUR;  // learned, saved on shutdown
    r = boot(slot, seed_a);
    check(r.source == TuningSource::RESTORED && near(r.value, 1.2f * PER_HOUR, 1e-7f),
          "lifecycle: an unchanged flash keeps what was learned");
    r = boot(slot, seed_a);
    check(r.source == TuningSource::RESTORED && near(r.value, 1.2f * PER_HOUR, 1e-7f),
          "lifecycle: and keeps it on every later flash");

    const float seed_b = workflow::pid::configured_seed(1.2f * PER_HOUR, 0.9f, config);
    r = boot(slot, seed_b);
    check(r.source == TuningSource::SEEDED && near(r.value, 1.2f * PER_HOUR, 1e-7f),
          "lifecycle: committing the learned rate as the seed changes nothing in use");
    slot.rate = 1.4f * PER_HOUR;
    r = boot(slot, seed_b);
    check(r.source == TuningSource::RESTORED && near(r.value, 1.4f * PER_HOUR, 1e-7f),
          "lifecycle: learning continues from the committed seed");

    // reset_tuning: the rate returns to the seed and the seed is stored with it.
    slot.rate = seed_b;
    slot.seed = seed_b;
    r = boot(slot, seed_b);
    check(r.source == TuningSource::RESTORED && near(r.value, seed_b, 1e-7f), "lifecycle: a reset survives a reboot");
  }

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all checks passed\n");
  return EXIT_SUCCESS;
}
