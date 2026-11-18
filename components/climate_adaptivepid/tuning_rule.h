#pragma once

#include <cmath>

// The PI gains that follow from the room model (room_model.h). No ESPHome
// dependency, so it can be tested and replayed natively.
//
// The PID's input is a setpoint offset. The device reaches full output once
// its setpoint is `full_output_gap` degrees past the room temperature, and at
// full output the room moves at `rate` degrees per second (the room model's
// heating or cooling rate, with heat loss taken out). As seen by the PID the
// room is then an integrating process with
//
//   process gain k' = rate / full_output_gap        [1/s]
//
// and the SIMC tuning rule for an integrating process gives
//
//   kp = 1 / (k' * tc)        ki = kp / (4 * tc)        tc = response_time + dead_time
namespace workflow {
namespace pid {

struct TuningConfig {
  bool adaptive{true};
  float response_time_s{600.0f};
  // Configured dead time; the starting value for the learned one.
  float dead_time_s{300.0f};
  float full_output_gap{0.75f};
  float kp_min{0.25f};
  float kp_max{4.0f};
};

struct Gains {
  float kp;
  float ki;
};

// Heating and cooling rates at full output: 0.2 to 10 degrees per hour.
static constexpr float MIN_RATE = 0.2f / 3600.0f;
static constexpr float MAX_RATE = 10.0f / 3600.0f;
// Starting rate when nothing is configured: 2 degrees per hour.
static constexpr float DEFAULT_RATE = 2.0f / 3600.0f;

inline float clamp_rate(float rate) {
  if (!(rate >= MIN_RATE)) return MIN_RATE;  // also catches NaN
  return rate > MAX_RATE ? MAX_RATE : rate;
}

// `dead_time_s` is the learned dead time for the mode (config.dead_time_s is
// only its starting value).
inline Gains gains_from_rate(float rate, float dead_time_s, const TuningConfig &config) {
  const float tc = config.response_time_s + dead_time_s;
  float kp = config.full_output_gap / (rate * tc);
  if (!(kp >= config.kp_min)) kp = config.kp_min;  // also catches NaN
  if (kp > config.kp_max) kp = config.kp_max;
  return Gains{kp, kp / (4.0f * tc)};
}

inline Gains gains_from_rate(float rate, const TuningConfig &config) {
  return gains_from_rate(rate, config.dead_time_s, config);
}

inline float rate_from_kp(float kp, const TuningConfig &config) {
  return config.full_output_gap / (kp * (config.response_time_s + config.dead_time_s));
}

}  // namespace pid
}  // namespace workflow
