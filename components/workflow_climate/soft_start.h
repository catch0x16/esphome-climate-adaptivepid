#pragma once

#include <cmath>
#include <cstdint>

// Soft start: when heating (or cooling) demand begins, the device setpoint is
// capped `offset` degrees below the target (above it when cooling) and the cap
// climbs evenly towards the target over `hold`: offset / hold per second.
//
// Hand-off. The climb stops as soon as heat arrives: the room has risen
// HEAT_ARRIVED_RISE above its lowest reading since the soft start began (it
// does not rely on the device reporting that it runs). At that moment the PID
// takes over from the setpoint the device already has: it is given a bias of
// (cap - its own output), so its output does not jump, and from there it
// moves only as the room moves. The bias then fades to zero over `hold`,
// giving the PID back its full authority if the room stalls. Without the
// bias, the PID would take over carrying kp times the whole shortfall, which
// the slow start has made larger, just as the heat already on its way
// reaches the room.
//
// If heat has not arrived by the time the cap no longer holds the PID back,
// the PID simply continues below it. The cap keeps rising past the target at
// the same rate until it reaches the top of the output range, so there is no
// jump at the end. While the cap binds the PID's integral does not accumulate
// (AdaptivePID::set_output_cap).
//
// The offset is learned per mode from each soft start's overshoot: the room's
// peak past the target, from the start until SETTLE_AFTER_END_MS after the
// soft start ends, or SETTLE_AFTER_OFF_MS after the device is switched off,
// whichever comes first (at most MAX_OBSERVE_MS). More overshoot than
// ACCEPTABLE_OVERSHOOT starts the next one lower (a larger offset); no more
// than that starts it a little higher, so the offset settles at the smallest
// one that avoids overshoot.
//
// The cap and the bias are in "output" degrees relative to the target:
// positive asks for more heating when heating and more cooling when cooling,
// negative for less. No ESPHome dependency, so it can be tested natively.
namespace workflow {
namespace pid {

struct SoftStartConfig {
  bool enabled{false};
  float initial_offset{1.5f};        // degrees below the target a soft start begins, before learning
  uint32_t hold_ms{20 * 60 * 1000};  // how long the cap takes to climb to the target, and the bias to fade
};

class SoftStart {
 public:
  enum class Phase : uint8_t {
    IDLE,     // no soft start
    CLIMB,    // the cap climbs until heat arrives
    HANDOFF,  // the PID runs with a bias that fades to zero
  };

  static constexpr float MIN_OFFSET = 0.0f;
  static constexpr float MAX_OFFSET = 3.0f;
  // The climb rate never falls below one device step (0.5) per hold, so a
  // small offset still ends.
  static constexpr float MIN_CLIMB = 0.5f;
  // Heat has arrived once the room is this far above its lowest reading;
  // above the 0.05-0.1 jitter of a fused room reading.
  static constexpr float HEAT_ARRIVED_RISE = 0.15f;
  // Overshoot past the target that a soft start may leave, about half a device step.
  static constexpr float ACCEPTABLE_OVERSHOOT = 0.25f;
  // More overshoot than that raises the offset by this fraction of the excess.
  static constexpr float RAISE_GAIN = 0.5f;
  // No more than that lowers the offset by this much.
  static constexpr float LOWER_STEP = 0.1f;
  // The room keeps moving after the soft start ends and after the device stops.
  static constexpr uint32_t SETTLE_AFTER_END_MS = 30 * 60 * 1000;
  static constexpr uint32_t SETTLE_AFTER_OFF_MS = 10 * 60 * 1000;
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
  bool active() const { return this->phase_ != Phase::IDLE; }
  Phase phase() const { return this->phase_; }

  float offset(bool heating) const { return this->offset_[heating ? 1 : 0]; }
  void set_offset(bool heating, float value) { this->offset_[heating ? 1 : 0] = clamp_offset(value); }
  // Degrees per second the cap climbs for this offset.
  float climb_rate(bool heating) const {
    const float hold_s = this->config_.hold_ms > 0 ? this->config_.hold_ms / 1000.0f : 1.0f;
    const float climb = this->offset(heating) > MIN_CLIMB ? this->offset(heating) : MIN_CLIMB;
    return climb / hold_s;
  }
  // The overshoot of the last measured soft start, NaN before the first.
  float last_overshoot() const { return this->last_overshoot_; }
  uint32_t starts_learned() const { return this->starts_learned_; }

  // Demand began. A device that is already `operating` is not stopped: the
  // cap starts from `current_output`, the output it runs at relative to the
  // target. Otherwise it starts `offset` below the target.
  void arm(uint32_t now_ms, bool heating, float target, float room, bool operating, float current_output) {
    if (!this->config_.enabled) return;
    // A soft start that has ended is measured before the new one starts; one
    // interrupted by new demand is not this soft start's whole outcome.
    if (this->observing_) {
      if (this->ended_) {
        this->learn_();
      } else {
        this->observing_ = false;
      }
    }
    this->phase_ = Phase::CLIMB;
    this->armed_ms_ = now_ms;
    this->rate_per_s_ = this->climb_rate(heating);
    this->start_cap_ = operating && std::isfinite(current_output) ? current_output : -this->offset(heating);
    this->lowest_room_ = room;
    this->bias_ = 0.0f;

    this->observing_ = true;
    this->heating_ = heating;
    this->target_ = target;
    this->observe_start_ms_ = now_ms;
    this->peak_ = -INFINITY;
    this->ended_ = false;
    this->switched_off_ = false;
  }

  // The device was switched off: the soft start no longer applies. The
  // measurement carries on, since the room may still overshoot.
  void cancel() {
    this->phase_ = Phase::IDLE;
    this->bias_ = 0.0f;
  }

  // While climbing: whether heat has arrived (see the top). True once per soft start.
  bool heat_arrived(float room) {
    if (this->phase_ != Phase::CLIMB || !std::isfinite(room)) return false;
    const float ahead = this->heating_ ? room : -room;
    if (!std::isfinite(this->lowest_room_)) {
      this->lowest_room_ = room;
      return false;
    }
    const float lowest = this->heating_ ? this->lowest_room_ : -this->lowest_room_;
    if (ahead < lowest) {
      this->lowest_room_ = room;
      return false;
    }
    return ahead - lowest >= HEAT_ARRIVED_RISE;
  }

  // Hand over to the PID, whose own output (relative to the target, in output
  // degrees) is `pid_output`: from the cap the device has now. Returns the
  // bias given to the PID (0 when the PID already asks for no more than the
  // cap, in which case the soft start ends).
  float hand_off(uint32_t now_ms, float max_output, float pid_output) {
    if (this->phase_ != Phase::CLIMB) return 0.0f;
    const float cap = this->cap(now_ms, max_output);
    const float bias = std::isfinite(cap) && std::isfinite(pid_output) ? cap - pid_output : 0.0f;
    if (!(bias < 0.0f)) {
      this->phase_ = Phase::IDLE;
      return 0.0f;
    }
    this->phase_ = Phase::HANDOFF;
    this->handoff_ms_ = now_ms;
    this->bias_ = bias;
    return bias;
  }

  // The most output the PID may ask for now, in output degrees relative to the
  // target; +infinity outside the climb. Once the cap reaches `max_output`
  // the soft start ends.
  float cap(uint32_t now_ms, float max_output) {
    if (this->phase_ != Phase::CLIMB) return INFINITY;
    const float cap = this->start_cap_ + this->rate_per_s_ * ((now_ms - this->armed_ms_) / 1000.0f);
    if (cap >= max_output) {
      this->phase_ = Phase::IDLE;
      return INFINITY;
    }
    return cap;
  }

  // The bias for the PID now, in output degrees: fades from the hand-off bias
  // to zero over `hold`, then the soft start ends. 0 outside the hand-off.
  float bias(uint32_t now_ms) {
    if (this->phase_ != Phase::HANDOFF) return 0.0f;
    const uint32_t elapsed = now_ms - this->handoff_ms_;
    if (this->config_.hold_ms == 0 || elapsed >= this->config_.hold_ms) {
      this->phase_ = Phase::IDLE;
      this->bias_ = 0.0f;
      return 0.0f;
    }
    return this->bias_ * (1.0f - static_cast<float>(elapsed) / this->config_.hold_ms);
  }

  // Feed every run. `max_offset` is how far below the target the setpoint may
  // go (max_adjustment_under), the most the learned offset may grow to.
  // Returns true when the measurement ended and the offset was learned.
  bool observe(uint32_t now_ms, bool heating, float room, float target, bool power_on, float max_offset) {
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
    if (!this->active() && !this->ended_) {
      this->ended_ = true;
      this->ended_ms_ = now_ms;
    }
    if (!power_on && !this->switched_off_) {
      this->switched_off_ = true;
      this->switched_off_ms_ = now_ms;
    } else if (power_on) {
      this->switched_off_ = false;
    }
    const bool settled = (this->ended_ && now_ms - this->ended_ms_ >= SETTLE_AFTER_END_MS) ||
                         (this->switched_off_ && now_ms - this->switched_off_ms_ >= SETTLE_AFTER_OFF_MS) ||
                         now_ms - this->observe_start_ms_ >= MAX_OBSERVE_MS;
    if (settled) {
      this->learn_();
      return true;
    }
    return false;
  }

  // Forget a measurement in progress (e.g. after a gap in the runs).
  void discard_measurement() { this->observing_ = false; }

 private:
  void stop_() {
    this->phase_ = Phase::IDLE;
    this->bias_ = 0.0f;
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

  Phase phase_{Phase::IDLE};
  // The climb.
  uint32_t armed_ms_{0};
  float start_cap_{0.0f};
  float rate_per_s_{0.0f};
  float lowest_room_{NAN};
  // The hand-off.
  uint32_t handoff_ms_{0};
  float bias_{0.0f};

  // The measurement of the current soft start's overshoot.
  bool observing_{false};
  bool heating_{true};
  float target_{NAN};
  uint32_t observe_start_ms_{0};
  float peak_{-INFINITY};
  bool ended_{false};
  uint32_t ended_ms_{0};
  bool switched_off_{false};
  uint32_t switched_off_ms_{0};
};

}  // namespace pid
}  // namespace workflow
