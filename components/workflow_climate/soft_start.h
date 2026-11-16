#pragma once

#include <cmath>
#include <cstdint>

// Soft start: when heating (or cooling) demand begins, the PID's output is
// capped just below the point at which the device starts operating, held there
// for `hold`, and the cap then rises `rate` per second until it no longer
// binds. Below the cap the PID runs as usual, and while the cap binds its
// integral does not accumulate.
//
// The device's own controller decides when to run, and how hard, from how far
// its setpoint is past its room reading. Starting from just below the point
// where it starts and raising the setpoint gradually lets it start at its
// weakest call, instead of at the full proportional correction.
//
// That start point is learned, per mode, as the device's "call" at the moment
// it begins operating during a soft start:
//
//   call = setpoint - room   (heating)      call = room - setpoint   (cooling)
//
// Each start moves the estimate SMOOTHING of the way. A device that starts as
// soon as it is switched on, wherever its setpoint is, starts during the hold:
// the estimate then moves down, and the next soft start begins lower.
//
// The cap is in "output" degrees relative to the target: positive asks for
// more heating when heating and more cooling when cooling, negative for less.
// No ESPHome dependency, so it can be tested natively.
namespace workflow {
namespace pid {

struct SoftStartConfig {
  bool enabled{false};
  float initial_start_point{0.0f};  // seed of the learned start point, degrees of call
  float margin{0.5f};               // how far below the start point a soft start begins
  uint32_t hold_ms{4 * 60 * 1000};
  float rate_per_s{0.1f / 60.0f};   // degrees per second the cap rises after the hold
};

class SoftStart {
 public:
  static constexpr float SMOOTHING = 0.3f;
  static constexpr float MIN_START_POINT = -3.0f;
  static constexpr float MAX_START_POINT = 3.0f;

  static float clamp_start_point(float value) {
    if (!(value >= MIN_START_POINT)) return MIN_START_POINT;  // also catches NaN
    return value > MAX_START_POINT ? MAX_START_POINT : value;
  }

  void configure(const SoftStartConfig &config) {
    this->config_ = config;
    this->start_point_[0] = this->start_point_[1] = clamp_start_point(config.initial_start_point);
    if (!config.enabled) this->active_ = false;
  }
  const SoftStartConfig &config() const { return this->config_; }

  void set_enabled(bool enabled) {
    this->config_.enabled = enabled;
    if (!enabled) this->active_ = false;
  }
  bool enabled() const { return this->config_.enabled; }
  bool active() const { return this->active_; }

  float start_point(bool heating) const { return this->start_point_[heating ? 1 : 0]; }
  void set_start_point(bool heating, float value) { this->start_point_[heating ? 1 : 0] = clamp_start_point(value); }
  uint32_t starts_learned() const { return this->starts_learned_; }

  // Demand began. `error` is how far the room is short of the target in the
  // output direction (positive: more output wanted). A device that is already
  // `operating` is not stopped: the cap starts from `current_output`, the
  // output it is running at relative to the target, and its start, which has
  // already happened, is not learned from.
  void arm(uint32_t now_ms, bool heating, float error, bool operating, float current_output) {
    if (!this->config_.enabled) return;
    this->active_ = true;
    this->heating_ = heating;
    this->armed_ms_ = now_ms;
    this->observe_start_ = !operating;
    if (operating && std::isfinite(current_output)) {
      this->start_cap_ = current_output;
    } else {
      // The cap is relative to the target; the call is relative to the room.
      this->start_cap_ = this->start_point(heating) - this->config_.margin - error;
    }
  }

  void cancel() { this->active_ = false; }

  // The most output the PID may ask for now, in output degrees relative to the
  // target; +infinity when no ramp is running. Once the cap reaches
  // `max_output` the ramp ends.
  float cap(uint32_t now_ms, float max_output) {
    if (!this->active_) return INFINITY;
    const uint32_t elapsed = now_ms - this->armed_ms_;
    float cap = this->start_cap_;
    if (elapsed > this->config_.hold_ms) {
      cap += this->config_.rate_per_s * ((elapsed - this->config_.hold_ms) / 1000.0f);
    }
    if (cap >= max_output) {
      this->active_ = false;
      return INFINITY;
    }
    return cap;
  }

  // The device began operating with this call (see the top). Learns the start
  // point when the start happened during a soft start the device was not
  // already running at. Returns true when the estimate was updated.
  bool on_device_start(bool heating, float call) {
    if (!this->active_ || !this->observe_start_ || heating != this->heating_ || !std::isfinite(call)) {
      return false;
    }
    this->observe_start_ = false;
    float &point = this->start_point_[heating ? 1 : 0];
    point = clamp_start_point(point + SMOOTHING * (call - point));
    this->starts_learned_++;
    return true;
  }

 private:
  SoftStartConfig config_{};
  float start_point_[2]{0.0f, 0.0f};  // 0 = cooling, 1 = heating
  uint32_t starts_learned_{0};

  bool active_{false};
  bool heating_{true};
  bool observe_start_{false};
  uint32_t armed_ms_{0};
  float start_cap_{0.0f};
};

}  // namespace pid
}  // namespace workflow
