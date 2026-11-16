#pragma once

#include <cmath>
#include <cstdint>

// Soft start: when heating (or cooling) demand begins, the device setpoint is
// capped `offset` degrees below the target (above it when cooling), held there
// for `hold`, and the cap then rises `rate` per second until it reaches the top
// of the output range. The device gets the lower of the PID's setpoint and the
// cap, so the PID takes over as soon as it asks for less. While the cap binds
// the PID's integral does not accumulate (AdaptivePID::set_output_cap), so
// nothing jumps when the cap rises past it.
//
// The offset is learned per mode from each soft start's overshoot: how far the
// room went past the target, from the start until the device has stopped and
// SETTLE_MS more have passed. More overshoot than ACCEPTABLE_OVERSHOOT starts
// the next one lower (a larger offset); no more than that starts it a little
// higher, so the offset settles at the smallest one that avoids overshoot.
//
// The cap is in "output" degrees relative to the target: positive asks for
// more heating when heating and more cooling when cooling, negative for less.
// No ESPHome dependency, so it can be tested natively.
namespace workflow {
namespace pid {

struct SoftStartConfig {
  bool enabled{false};
  float initial_offset{1.5f};       // degrees below the target a soft start begins, before learning
  uint32_t hold_ms{4 * 60 * 1000};
  float rate_per_s{0.1f / 60.0f};   // degrees per second the cap rises after the hold
};

class SoftStart {
 public:
  static constexpr float MIN_OFFSET = 0.0f;
  static constexpr float MAX_OFFSET = 3.0f;
  // Overshoot past the target that a soft start may leave, about half a device step.
  static constexpr float ACCEPTABLE_OVERSHOOT = 0.25f;
  // More overshoot than that raises the offset by this fraction of the excess.
  static constexpr float RAISE_GAIN = 0.5f;
  // No more than that lowers the offset by this much.
  static constexpr float LOWER_STEP = 0.1f;
  // After the device stops, the room keeps rising for a while.
  static constexpr uint32_t SETTLE_MS = 10 * 60 * 1000;
  // A measurement ends after this long, whether or not the device has stopped.
  static constexpr uint32_t MAX_OBSERVE_MS = 90 * 60 * 1000;

  static float clamp_offset(float value) {
    if (!(value >= MIN_OFFSET)) return MIN_OFFSET;  // also catches NaN
    return value > MAX_OFFSET ? MAX_OFFSET : value;
  }

  void configure(const SoftStartConfig &config) {
    this->config_ = config;
    this->offset_[0] = this->offset_[1] = clamp_offset(config.initial_offset);
    if (!config.enabled) this->stop_();
  }
  const SoftStartConfig &config() const { return this->config_; }

  void set_enabled(bool enabled) {
    this->config_.enabled = enabled;
    if (!enabled) this->stop_();
  }
  bool enabled() const { return this->config_.enabled; }
  bool active() const { return this->active_; }

  float offset(bool heating) const { return this->offset_[heating ? 1 : 0]; }
  void set_offset(bool heating, float value) { this->offset_[heating ? 1 : 0] = clamp_offset(value); }
  // The overshoot of the last measured soft start, NaN before the first.
  float last_overshoot() const { return this->last_overshoot_; }
  uint32_t starts_learned() const { return this->starts_learned_; }

  // Demand began. A device that is already `operating` is not stopped: the
  // cap starts from `current_output`, the output it runs at relative to the
  // target. Otherwise it starts `offset` below the target.
  void arm(uint32_t now_ms, bool heating, float target, bool operating, float current_output) {
    if (!this->config_.enabled) return;
    this->active_ = true;
    this->armed_ms_ = now_ms;
    this->start_cap_ = operating && std::isfinite(current_output) ? current_output : -this->offset(heating);

    // A soft start that has run and stopped is measured before the new one starts.
    if (this->observing_ && this->ran_ && this->stopped_) {
      this->learn_();
    }
    this->observing_ = true;
    this->heating_ = heating;
    this->target_ = target;
    this->observe_start_ms_ = now_ms;
    this->peak_ = -INFINITY;
    this->ran_ = operating;
    this->stopped_ = false;
  }

  // The cap no longer applies (the device was switched off); a measurement in
  // progress carries on, since the room may still overshoot.
  void cancel() { this->active_ = false; }

  // The most output the PID may ask for now, in output degrees relative to the
  // target; +infinity when no soft start is running. Once the cap reaches
  // `max_output` the soft start ends.
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

  // Feed every run. `max_offset` is how far below the target the setpoint may
  // go (max_adjustment_under), the most the learned offset may grow to.
  // Returns true when the measurement ended and the offset was learned.
  bool observe(uint32_t now_ms, bool heating, float room, float target, bool operating, float max_offset) {
    if (!this->observing_) return false;
    if (heating != this->heating_ || !(std::fabs(target - this->target_) <= 0.01f)) {
      this->observing_ = false;  // the target or mode changed: not this soft start's outcome
      return false;
    }
    this->max_offset_ = max_offset;
    if (std::isfinite(room)) {
      const float overshoot = heating ? room - target : target - room;
      if (overshoot > this->peak_) this->peak_ = overshoot;
    }
    if (operating) {
      this->ran_ = true;
      this->stopped_ = false;
    } else if (this->ran_ && !this->stopped_) {
      this->stopped_ = true;
      this->stopped_ms_ = now_ms;
    }
    const bool settled = this->stopped_ && now_ms - this->stopped_ms_ >= SETTLE_MS;
    const bool expired = now_ms - this->observe_start_ms_ >= MAX_OBSERVE_MS;
    if (settled || (expired && this->ran_)) {
      this->learn_();
      return true;
    }
    if (expired) {
      this->observing_ = false;  // the device never ran: nothing to learn
    }
    return false;
  }

  // Forget a measurement in progress (e.g. after a gap in the runs).
  void discard_measurement() { this->observing_ = false; }

 private:
  void stop_() {
    this->active_ = false;
    this->observing_ = false;
  }

  void learn_() {
    this->observing_ = false;
    if (!std::isfinite(this->peak_)) return;
    float &offset = this->offset_[this->heating_ ? 1 : 0];
    if (this->peak_ > ACCEPTABLE_OVERSHOOT) {
      offset += RAISE_GAIN * (this->peak_ - ACCEPTABLE_OVERSHOOT);
    } else {
      offset -= LOWER_STEP;
    }
    offset = clamp_offset(offset);
    if (std::isfinite(this->max_offset_) && offset > this->max_offset_) {
      offset = this->max_offset_ > MIN_OFFSET ? this->max_offset_ : MIN_OFFSET;
    }
    this->last_overshoot_ = this->peak_;
    this->starts_learned_++;
  }

  SoftStartConfig config_{};
  float offset_[2]{1.5f, 1.5f};  // 0 = cooling, 1 = heating
  float last_overshoot_{NAN};
  uint32_t starts_learned_{0};
  float max_offset_{NAN};

  // The cap.
  bool active_{false};
  uint32_t armed_ms_{0};
  float start_cap_{0.0f};

  // The measurement of the current soft start's overshoot.
  bool observing_{false};
  bool heating_{true};
  float target_{NAN};
  uint32_t observe_start_ms_{0};
  float peak_{-INFINITY};
  bool ran_{false};
  bool stopped_{false};
  uint32_t stopped_ms_{0};
};

}  // namespace pid
}  // namespace workflow
