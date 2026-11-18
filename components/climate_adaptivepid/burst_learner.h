#pragma once

#include <cmath>
#include <cstdint>

#include "start_controller.h"

// Learns, per run ("burst") and per shared-heat episode, what start control
// needs. No ESPHome dependency, so it can be tested natively
// (tests/test_burst_learner.cpp). See docs/start-control-design.md.
//
// - Gain: the room's peak after a run minus the room at its start. A start
//   is worth this much, so starting half of it below target lands the peak
//   about half of it above.
// - Coast: the peak minus the room when the run stopped, and how long after
//   the stop the peak came. Predicts the peak while running (early stop), and
//   is the window in which a rising room is the head's own heat, not shared.
//
// Gain and coast are kept apart for runs alone and runs while other heads
// also ran (shared), since a shared outdoor unit keeps delivering after this
// head stops. Runs whose coast was cut short (the device switched off, a new
// start) are discarded: they would understate the coast.
namespace workflow {
namespace start {

struct BurstState {
  float gain[2];      // C, [0] alone, [1] shared
  float coast[2];     // C
  float coast_time_s;
  uint32_t gain_count[2];
};

class BurstLearner {
 public:
  static constexpr float SMOOTHING = 0.3f;
  // A coast ends once the room has moved back this far from its peak.
  static constexpr float COAST_END = 0.1f;
  static constexpr uint32_t MAX_COAST_MS = 15 * 60 * 1000;
  // Defaults while nothing is learned.
  static constexpr float DEFAULT_COAST_ALONE = 0.3f;
  static constexpr float DEFAULT_COAST_SHARED = 0.6f;
  static constexpr uint32_t DEFAULT_COAST_TIME_MS = 5 * 60 * 1000;
  // Runs needed before a learned value is used.
  static constexpr uint32_t MIN_RUNS = 3;

  BurstLearner() { this->reset(); }

  void reset() {
    this->state_ = BurstState{{NAN, NAN}, {NAN, NAN}, NAN, {0, 0}};
    this->phase_ = Phase::NONE;
  }
  void set_state(const BurstState &state) { this->state_ = state; }
  const BurstState &state() const { return this->state_; }

  // Called every update with the controller's state after the update.
  // Returns true when something was learned.
  bool update(uint32_t now_ms, bool heating, HeadState state, bool powered, float room, bool shared) {
    bool learned = false;
    const float sign = heating ? 1.0f : -1.0f;
    if (heating != this->heating_) {
      this->heating_ = heating;
      this->phase_ = Phase::NONE;
    }
    const bool running = state == HeadState::START || state == HeadState::RUN;

    switch (this->phase_) {
      case Phase::NONE:
        break;
      case Phase::RUNNING:
        if (state == HeadState::PARKED || !powered) {
          this->phase_ = Phase::NONE;
        } else if (!running) {
          if (this->saw_run_) {
            this->phase_ = Phase::COASTING;
            this->stop_ms_ = now_ms;
            this->stop_room_ = room;
            this->peak_room_ = room;
            this->peak_ms_ = now_ms;
          } else {
            this->phase_ = Phase::NONE;  // the unit never started
          }
        } else {
          this->saw_run_ |= state == HeadState::RUN;
          this->shared_ |= shared;
        }
        break;
      case Phase::COASTING:
        if (state == HeadState::PARKED || !powered || running) {
          this->phase_ = Phase::NONE;  // cut short
          break;
        }
        this->shared_ |= shared;
        if (std::isfinite(room) && sign * (room - this->peak_room_) > 0.0f) {
          this->peak_room_ = room;
          this->peak_ms_ = now_ms;
        }
        if ((std::isfinite(room) && sign * (this->peak_room_ - room) >= COAST_END) ||
            static_cast<uint32_t>(now_ms - this->stop_ms_) >= MAX_COAST_MS) {
          learned |= this->record_burst_(sign);
          this->phase_ = Phase::NONE;
        }
        break;
    }
    if (this->phase_ == Phase::NONE && running && std::isfinite(room)) {
      this->phase_ = Phase::RUNNING;
      this->start_room_ = room;
      this->saw_run_ = state == HeadState::RUN;
      this->shared_ = shared;
    }

    return learned;
  }

  bool coasting() const { return this->phase_ == Phase::COASTING; }

  float coast(bool shared) const {
    const int i = shared ? 1 : 0;
    if (this->state_.gain_count[i] >= MIN_RUNS && std::isfinite(this->state_.coast[i])) {
      return this->state_.coast[i];
    }
    return shared ? DEFAULT_COAST_SHARED : DEFAULT_COAST_ALONE;
  }
  uint32_t coast_time_ms() const {
    const uint32_t runs = this->state_.gain_count[0] + this->state_.gain_count[1];
    if (runs >= MIN_RUNS && std::isfinite(this->state_.coast_time_s)) {
      const float s = this->state_.coast_time_s < 60.0f ? 60.0f
                      : this->state_.coast_time_s > 900.0f ? 900.0f
                                                           : this->state_.coast_time_s;
      return static_cast<uint32_t>(s * 1000.0f);
    }
    return DEFAULT_COAST_TIME_MS;
  }
  // Learned gain of a run alone, C; NaN until enough runs.
  float gain_alone() const {
    return this->state_.gain_count[0] >= MIN_RUNS ? this->state_.gain[0] : NAN;
  }

 private:
  enum class Phase : uint8_t { NONE, RUNNING, COASTING };

  static void smooth_(float &value, float sample) {
    value = std::isfinite(value) ? value + SMOOTHING * (sample - value) : sample;
  }

  bool record_burst_(float sign) {
    if (!std::isfinite(this->start_room_) || !std::isfinite(this->stop_room_) ||
        !std::isfinite(this->peak_room_)) {
      return false;
    }
    const int i = this->shared_ ? 1 : 0;
    const float gain = sign * (this->peak_room_ - this->start_room_);
    const float coast = sign * (this->peak_room_ - this->stop_room_);
    if (!(gain > 0.0f) || coast < 0.0f) {
      return false;
    }
    smooth_(this->state_.gain[i], gain);
    smooth_(this->state_.coast[i], coast);
    smooth_(this->state_.coast_time_s, (this->peak_ms_ - this->stop_ms_) / 1000.0f);
    this->state_.gain_count[i]++;
    return true;
  }

  BurstState state_{};
  bool heating_{true};
  Phase phase_{Phase::NONE};
  bool saw_run_{false};
  bool shared_{false};
  float start_room_{NAN};
  uint32_t stop_ms_{0};
  float stop_room_{NAN};
  float peak_room_{NAN};
  uint32_t peak_ms_{0};
};

}  // namespace start
}  // namespace workflow
