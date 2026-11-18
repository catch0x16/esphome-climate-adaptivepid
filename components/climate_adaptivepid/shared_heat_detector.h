#pragma once

#include <cmath>
#include <cstdint>

// Detects heat (or cooling) arriving from other heads on a shared outdoor unit.
// No ESPHome dependency, so it can be tested natively
// (tests/test_shared_heat_detector.cpp). See docs/start-control-design.md.
//
// A powered indoor head whose own compressor is idle still receives refrigerant
// while another head on the same outdoor unit runs, and its fan carries that
// heat into the room. The units do not report the outdoor unit's state, so the
// head infers it from the room: while it is powered and idle, and outside its
// own coast after a run, a room that keeps warming (cooling, in cool mode) with
// humidity falling is receiving heat from elsewhere.
//
// A supervisor that sees every head knows for certain. Its hint, while valid,
// replaces the local detector; once it expires the detector decides again.
namespace workflow {
namespace start {

class SharedHeatDetector {
 public:
  // One sample is kept per SAMPLE_MS, enough for the slope window.
  static constexpr uint32_t SAMPLE_MS = 10000;
  static constexpr int CAPACITY = 24;  // 4 minutes

  struct Config {
    uint32_t window_ms{120000};
    // Moving the way output would move it at least this fast: shared heat.
    float rise_per_min{0.05f};
    // Humidity must fall at least this much over the window (heating only),
    // when a humidity reading is available.
    float humidity_drop{0.2f};
    // Shared heat ends once the room has stopped moving: at most this over the window.
    float settled_per_min{0.01f};
    // The room counts as rising (for the start decision) above this.
    float rising_per_min{0.02f};
  };

  void configure(const Config &config) { this->config_ = config; }

  // eligible: powered, the head idle, and past its own coast after a run.
  void update(uint32_t now_ms, bool heating, bool eligible, float room, float humidity) {
    this->heating_ = heating;
    this->add_sample_(now_ms, room, humidity);
    const float slope = this->slope_per_min_(now_ms);
    if (!eligible || !std::isfinite(slope)) {
      this->local_ = false;
      return;
    }
    if (!this->local_) {
      const float drop = this->humidity_drop_(now_ms);
      const bool humidity_agrees = !heating || !std::isfinite(drop) || drop >= this->config_.humidity_drop;
      this->local_ = slope >= this->config_.rise_per_min && humidity_agrees;
    } else if (slope <= this->config_.settled_per_min) {
      this->local_ = false;
    }
  }

  // The supervisor's view, valid for valid_ms from now.
  void set_hint(bool active, uint32_t now_ms, uint32_t valid_ms) {
    this->has_hint_ = valid_ms > 0;
    this->hint_ = active;
    this->hint_from_ms_ = now_ms;
    this->hint_valid_ms_ = valid_ms;
  }
  bool hint_valid(uint32_t now_ms) const {
    return this->has_hint_ && static_cast<uint32_t>(now_ms - this->hint_from_ms_) < this->hint_valid_ms_;
  }

  bool local() const { return this->local_; }
  bool active(uint32_t now_ms) const { return this->hint_valid(now_ms) ? this->hint_ : this->local_; }

  // The room is moving the way output would move it (warming when heating).
  bool rising(uint32_t now_ms) const {
    const float slope = this->slope_per_min_(now_ms);
    return std::isfinite(slope) && slope >= this->config_.rising_per_min;
  }
  // The slope over the window in the direction of output, C/min; NaN without enough history.
  float slope_per_min(uint32_t now_ms) const { return this->slope_per_min_(now_ms); }

 private:
  struct Sample {
    uint32_t ms;
    float room;
    float humidity;
  };

  void add_sample_(uint32_t now_ms, float room, float humidity) {
    if (!std::isfinite(room)) {
      return;
    }
    if (this->count_ > 0) {
      const Sample &last = this->samples_[(this->head_ + CAPACITY - 1) % CAPACITY];
      if (static_cast<uint32_t>(now_ms - last.ms) < SAMPLE_MS) {
        // Keep the latest reading for "now" without adding a sample.
        this->now_room_ = room;
        this->now_humidity_ = humidity;
        this->now_ms_ = now_ms;
        return;
      }
    }
    this->samples_[this->head_] = Sample{now_ms, room, humidity};
    this->head_ = (this->head_ + 1) % CAPACITY;
    if (this->count_ < CAPACITY) {
      this->count_++;
    }
    this->now_room_ = room;
    this->now_humidity_ = humidity;
    this->now_ms_ = now_ms;
  }

  // The newest sample at least window_ms old, or nullptr.
  const Sample *window_start_(uint32_t now_ms) const {
    for (int i = 1; i <= this->count_; i++) {
      const Sample &s = this->samples_[(this->head_ + CAPACITY - i) % CAPACITY];
      if (static_cast<uint32_t>(now_ms - s.ms) >= this->config_.window_ms) {
        return &s;
      }
    }
    return nullptr;
  }

  float slope_per_min_(uint32_t now_ms) const {
    const Sample *start = this->window_start_(now_ms);
    if (start == nullptr || !std::isfinite(this->now_room_)) {
      return NAN;
    }
    const float minutes = static_cast<uint32_t>(this->now_ms_ - start->ms) / 60000.0f;
    if (!(minutes > 0.0f)) {
      return NAN;
    }
    const float change = (this->now_room_ - start->room) / minutes;
    return this->heating_ ? change : -change;
  }

  float humidity_drop_(uint32_t now_ms) const {
    const Sample *start = this->window_start_(now_ms);
    if (start == nullptr || !std::isfinite(start->humidity) || !std::isfinite(this->now_humidity_)) {
      return NAN;
    }
    return start->humidity - this->now_humidity_;
  }

  Config config_{};
  Sample samples_[CAPACITY]{};
  int head_{0};
  int count_{0};
  float now_room_{NAN};
  float now_humidity_{NAN};
  uint32_t now_ms_{0};
  bool heating_{true};
  bool local_{false};
  bool has_hint_{false};
  bool hint_{false};
  uint32_t hint_from_ms_{0};
  uint32_t hint_valid_ms_{0};
};

}  // namespace start
}  // namespace workflow
