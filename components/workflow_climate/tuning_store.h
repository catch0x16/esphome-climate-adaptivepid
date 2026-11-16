#pragma once

#include <cmath>
#include <cstdint>

#include "tuning_rule.h"

// Whether each learned model value (room_model.h) starts from what was learned
// or from its configured seed at boot. No ESPHome dependency, so it can be
// tested natively (tests/test_tuning_store.cpp).
//
// Every learned value is stored together with the configured seed it was
// learned from. At boot:
//   - stored seed == configured seed: nothing was changed on purpose, so the
//     learned value is kept (RESTORED).
//   - the seeds differ, or nothing usable is stored: the configuration was
//     changed on purpose, so the configured seed is used (SEEDED).
// Editing a seed in YAML therefore takes effect on the next flash, and leaving
// it alone keeps whatever the unit has learned.
namespace workflow {
namespace pid {

enum class TuningSource : uint8_t {
  // Not CONFIGURED: ESP-IDF builds define CONFIGURED as a macro.
  SEEDED,    // started from the configured seed
  RESTORED,  // started from the value learned before the last reboot
};

inline const char *tuning_source_to_string(TuningSource source) {
  return source == TuningSource::RESTORED ? "restored" : "configured";
}

// The rate seed of a mode: `initial_rate` (degrees per second) when it is
// configured, else the rate implied by a configured kp, else the default.
// Deterministic, so the same configuration gives the same seed on every boot.
inline float configured_seed(float initial_rate, float kp, const TuningConfig &config) {
  if (std::isfinite(initial_rate) && initial_rate > 0.0f) {
    return clamp_rate(initial_rate);
  }
  if (kp > 0.0f) {
    return clamp_rate(rate_from_kp(kp, config));
  }
  return DEFAULT_RATE;
}

// Seeds come from the same compiled constants on every boot; the tolerance
// only absorbs float noise.
inline bool same_seed(float a, float b) {
  if (!std::isfinite(a) || !std::isfinite(b)) {
    return false;
  }
  return std::fabs(a - b) <= 1e-4f * std::fmax(std::fabs(a), std::fabs(b));
}

struct Resolved {
  float value;
  TuningSource source;
};

// A stored value below `low` (or NaN) is not usable; a restored one is kept
// inside [low, high].
inline Resolved resolve(float stored, float stored_seed, float configured, float low, float high) {
  const bool usable = std::isfinite(stored) && stored >= low;
  if (usable && same_seed(stored_seed, configured)) {
    return Resolved{stored > high ? high : stored, TuningSource::RESTORED};
  }
  return Resolved{configured, TuningSource::SEEDED};
}

// A heating or cooling rate.
inline Resolved resolve(float stored_rate, float stored_seed, float configured) {
  return resolve(stored_rate, stored_seed, configured, MIN_RATE, MAX_RATE);
}

}  // namespace pid
}  // namespace workflow
