#pragma once

#include <cmath>
#include <cstdint>

// Shapes the PID's setpoint before it goes to the device, so that moves towards
// more output are slow and moves towards less output are fast. No ESPHome
// dependency, so it can be tested natively (tests/test_output_shaper.cpp).
//
// The units' own thermostat works against the temperature they report (the
// remote room temperature, rounded to their 0.5 C step): heating starts once the
// setpoint reaches it, keeps running at a setpoint 0.5 below it, and stops at 1.0
// below. Left alone, the PID steps the setpoint down one step at a time as the
// room rises, so a small room overshoots before the unit stops.
//
// - run_backoff: once the device starts, its setpoint is held at most this far
//   below the temperature it reported when it started (above, when cooling). At
//   0.5 the device keeps running at its lowest demand, and stops by itself as
//   soon as the temperature it sees moves one step.
// - increase_rate: moves towards more output are limited to this many degrees
//   per second. Moves towards less output are never limited.
//
// Both are off at 0.
namespace workflow {
namespace pid {

class OutputShaper {
 public:
  void configure(float run_backoff, float increase_rate_per_s) {
    this->run_backoff_ = run_backoff > 0.0f ? run_backoff : 0.0f;
    this->increase_rate_ = increase_rate_per_s > 0.0f ? increase_rate_per_s : 0.0f;
  }

  bool enabled() const { return this->run_backoff_ > 0.0f || this->increase_rate_ > 0.0f; }

  // requested: the PID's setpoint. device_temperature: the temperature the
  // device reports. Returns the setpoint to send.
  float shape(float requested, bool heating, bool powered, bool operating, float device_temperature, uint32_t now_ms) {
    if (!powered || !std::isfinite(requested)) {
      this->reset();
      return requested;
    }
    if (heating != this->heating_) {
      this->reset();
      this->heating_ = heating;
    }

    if (operating && !this->running_) {
      this->running_ = true;
      this->start_temperature_ = device_temperature;
    } else if (!operating) {
      this->running_ = false;
    }

    float value = requested;
    if (this->running_ && this->run_backoff_ > 0.0f && std::isfinite(this->start_temperature_)) {
      const float cap = heating ? this->start_temperature_ - this->run_backoff_
                                : this->start_temperature_ + this->run_backoff_;
      if (this->more_(value, cap)) {
        value = cap;
      }
    }
    if (this->increase_rate_ > 0.0f && std::isfinite(this->previous_)) {
      const float allowed = this->increase_rate_ * ((now_ms - this->previous_ms_) / 1000.0f);
      const float limit = heating ? this->previous_ + allowed : this->previous_ - allowed;
      if (this->more_(value, limit)) {
        value = limit;
      }
    }

    this->limiting_ = this->more_(requested, value);
    this->previous_ = value;
    this->previous_ms_ = now_ms;
    return value;
  }

  // The last shaped setpoint asked for less output than the PID did.
  bool limiting() const { return this->limiting_; }
  // The device is running and the start temperature is known.
  bool running() const { return this->running_; }
  float start_temperature() const { return this->start_temperature_; }

  // Forget the history: the next setpoint passes as requested.
  void reset() {
    this->running_ = false;
    this->start_temperature_ = NAN;
    this->previous_ = NAN;
    this->limiting_ = false;
  }

 private:
  // Whether setpoint a asks for more output than b.
  bool more_(float a, float b) const { return this->heating_ ? a > b : a < b; }

  float run_backoff_{0.0f};
  float increase_rate_{0.0f};

  bool heating_{true};
  bool running_{false};
  float start_temperature_{NAN};
  float previous_{NAN};
  uint32_t previous_ms_{0};
  bool limiting_{false};
};

}  // namespace pid
}  // namespace workflow
