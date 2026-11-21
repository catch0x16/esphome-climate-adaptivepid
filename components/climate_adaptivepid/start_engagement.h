#pragma once

#include <cmath>
#include <cstdint>

// When start control is engaged on a head in continuous mode
// (`start_control: continuous`). No ESPHome dependency, so it can be tested
// natively (tests/test_start_engagement.cpp). See docs/start-control-design.md.
//
// A continuous head runs the regular PID path, and switches to start control
// only while the supervisor says a head that ranks above it is running, or is
// expected to start within the forecast window. It goes back once neither has
// held for `release_ms` (the hysteresis min_cycle_duration) and the start
// controller is idle or parked, so a run it started finishes first.
//
// Only the supervisor's signals count, not the local detector: the heat a
// higher-ranked head leaks into a room can be well below what the detector
// needs, and a false start would change the head's behaviour for nothing.
namespace workflow {
namespace start {

// What `start_control` is set to.
enum class StartMode : uint8_t {
  OFF = 0,         // the regular PID path only
  DISCRETE = 1,    // start control always
  CONTINUOUS = 2,  // the regular PID path; start control while a higher-ranked head runs
};

inline const char *start_mode_to_string(StartMode mode) {
  switch (mode) {
    case StartMode::OFF:
      return "off";
    case StartMode::DISCRETE:
      return "discrete";
    case StartMode::CONTINUOUS:
      return "continuous";
  }
  return "unknown";
}

enum class EngageChange : uint8_t { NONE, ENGAGED, RELEASED };

class StartEngagement {
 public:
  void configure(uint32_t release_ms, uint32_t expect_window_s) {
    this->release_ms_ = release_ms;
    this->expect_window_s_ = expect_window_s;
  }

  bool engaged() const { return this->engaged_; }

  // The supervisor says shared heat is on, or a forecast falls within the window.
  bool signal(bool hint, float expected_s) const {
    return hint || (std::isfinite(expected_s) && expected_s <= static_cast<float>(this->expect_window_s_));
  }

  // Called every update. `settled`: the start controller is idle or parked.
  EngageChange update(uint32_t now_ms, bool signal, bool settled) {
    if (signal) {
      this->last_signal_ms_ = now_ms;
      if (!this->engaged_) {
        this->engaged_ = true;
        return EngageChange::ENGAGED;
      }
      return EngageChange::NONE;
    }
    if (this->engaged_ && settled &&
        static_cast<uint32_t>(now_ms - this->last_signal_ms_) >= this->release_ms_) {
      this->engaged_ = false;
      return EngageChange::RELEASED;
    }
    return EngageChange::NONE;
  }

 private:
  uint32_t release_ms_{60000};
  uint32_t expect_window_s_{300};
  bool engaged_{false};
  uint32_t last_signal_ms_{0};
};

}  // namespace start
}  // namespace workflow
