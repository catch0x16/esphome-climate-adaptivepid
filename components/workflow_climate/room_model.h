#pragma once

#include <cmath>
#include <cstdint>

#include "tuning_rule.h"

// Online model of how the room responds, learned while the controller runs.
// No ESPHome dependency, so it can be tested and replayed natively
// (tests/test_room_model.cpp, tools/room_model/replay.cpp).
//
//   dT/dt = b_heat * u                  - loss * (T_room - T_outdoor)   heating
//   dT/dt = -b_cool * u                 - loss * (T_room - T_outdoor)   cooling
//
// - loss: how fast the room drifts towards the outdoor temperature [1/s].
// - b_heat, b_cool: how fast the device moves the room at full output [C/s].
// - u: the device's output, 0..1. The units report no output (no compressor
//   frequency), so it is estimated from the setpoint the device was given:
//   the device runs at full output once its setpoint is full_output_gap past
//   the room temperature (estimate_output).
//
// The model is learned in fixed windows of uninterrupted operation or idling,
// so the 0.1 C steps of a room sensor average out:
// - Idle windows (device not operating) measure the drift, and with it loss.
// - Operating windows measure the change, take out the drift the loss
//   predicts, and divide by the mean output: one sample of b.
// A room held steady at low output therefore no longer reads as a slow room:
// windows below MIN_OUTPUT teach nothing about b.
//
// After every start or stop the room keeps reacting to the previous state for
// a while. That while is the dead time, learned per mode as the time from a
// start until the room turns: after a heating start the room usually keeps
// falling, and the turn is taken where it bottoms out (the middle of the time
// spent at the lowest reading, which with 0.1 C steps is a plateau), once it
// has since risen DEAD_TIME_RISE. Cooling is the mirror image. No window is
// taken inside the dead time.
//
// A defrost cycle, which the device reports, delivers no heat: no window is
// taken during one, and the model then waits a dead time again, as after a
// start, for the heat to arrive again.
//
// An operating window in which the room moves the wrong way faster than with
// the device off (a window opened, an unreported defrost) is dropped as an
// anomaly.
// So is one in which any CHECK_MS stretch does so by CHECK_MARGIN, and the
// model then waits a dead time again, as after a start: the heat a defrost
// interrupted takes that long to arrive again.
namespace workflow {
namespace pid {

// Estimated output, 0..1, of a device given `device_setpoint` while the room is
// at `room`. 0 while the device is not operating.
inline float estimate_output(bool operating, bool heating, float device_setpoint, float room, float full_output_gap) {
  if (!operating || !std::isfinite(device_setpoint) || !std::isfinite(room) || !(full_output_gap > 0.0f)) {
    return 0.0f;
  }
  const float gap = heating ? device_setpoint - room : room - device_setpoint;
  if (!(gap > 0.0f)) {
    return 0.0f;
  }
  return gap >= full_output_gap ? 1.0f : gap / full_output_gap;
}

struct ModelSample {
  uint32_t now_ms;
  float room;      // room temperature [C]
  float outdoor;   // outdoor temperature [C], NaN when unknown
  bool heating;    // the mode: heating (true) or cooling (false)
  bool operating;  // the device is actively heating or cooling as commanded
  float output;    // estimated output 0..1 (estimate_output)
  bool defrosting{false};  // the device reports a defrost cycle
};

// What is learned, and saved across reboots.
struct ModelState {
  float loss;         // [1/s]
  float heat_rate;    // [C/s] at full output
  float cool_rate;    // [C/s] at full output
  float heat_dead_s;  // [s]
  float cool_dead_s;  // [s]
};

// What one update changed.
struct ModelUpdate {
  bool rate{false};       // b of the sample's mode
  bool loss{false};
  bool dead_time{false};  // dead time of the sample's mode
  bool anomaly{false};    // an operating window was dropped
};

class RoomModel {
 public:
  // One sample of loss or b per this much uninterrupted idling or operation.
  static constexpr uint32_t WINDOW_MS = 15 * 60 * 1000;
  // Two samples further apart than this are not joined.
  static constexpr uint32_t MAX_SAMPLE_GAP_MS = 5 * 60 * 1000;
  // Each sample of b or dead time moves the estimate this fraction of the way.
  static constexpr float SMOOTHING = 0.2f;
  // Loss is slower to change, and idle windows are frequent.
  static constexpr float LOSS_SMOOTHING = 0.1f;
  // Operating windows below this mean output carry little signal about b.
  static constexpr float MIN_OUTPUT = 0.3f;
  // Idle windows closer than this to the outdoor temperature carry little signal about loss.
  static constexpr float MIN_LOSS_GAP = 3.0f;
  // 0 to 2 per hour; 0.1 per hour is a drift of 1 C/h at 10 C indoor-outdoor difference.
  static constexpr float MAX_LOSS = 2.0f / 3600.0f;
  static constexpr float DEFAULT_LOSS = 0.1f / 3600.0f;
  // How much faster than the device-off drift the room may move the wrong way
  // before an operating window is dropped: 0.3 C/h.
  static constexpr float ANOMALY_MARGIN = 0.3f / 3600.0f;
  // The same check on shorter stretches inside a window, with a margin above
  // what one 0.1 C sensor step over the stretch reads as (1.2 C/h).
  static constexpr uint32_t CHECK_MS = 5 * 60 * 1000;
  static constexpr float CHECK_MARGIN = 1.5f / 3600.0f;
  // Dead time: the room must move this far past its turn for the turn to count.
  static constexpr float DEAD_TIME_RISE = 0.15f;
  // Readings this close count as the same reading (smoothed sensors are not on 0.1 C steps).
  static constexpr float DEAD_TIME_SAME = 0.02f;
  static constexpr float MIN_DEAD_S = 60.0f;
  static constexpr float MAX_DEAD_S = 30.0f * 60.0f;

  static float clamp_loss(float loss) {
    if (!(loss >= 0.0f)) return 0.0f;  // also catches NaN
    return loss > MAX_LOSS ? MAX_LOSS : loss;
  }
  static float clamp_dead(float seconds) {
    if (!(seconds >= MIN_DEAD_S)) return MIN_DEAD_S;
    return seconds > MAX_DEAD_S ? MAX_DEAD_S : seconds;
  }

  void set_state(const ModelState &state) {
    this->state_.loss = clamp_loss(state.loss);
    this->state_.heat_rate = clamp_rate(state.heat_rate);
    this->state_.cool_rate = clamp_rate(state.cool_rate);
    this->state_.heat_dead_s = clamp_dead(state.heat_dead_s);
    this->state_.cool_dead_s = clamp_dead(state.cool_dead_s);
  }
  const ModelState &state() const { return this->state_; }

  float loss() const { return this->state_.loss; }
  float rate(bool heating) const { return heating ? this->state_.heat_rate : this->state_.cool_rate; }
  float dead_time_s(bool heating) const { return heating ? this->state_.heat_dead_s : this->state_.cool_dead_s; }

  uint32_t rate_measurements(bool heating) const { return heating ? this->heat_n_ : this->cool_n_; }
  uint32_t loss_measurements() const { return this->loss_n_; }
  uint32_t dead_time_measurements(bool heating) const { return heating ? this->heat_dead_n_ : this->cool_dead_n_; }
  uint32_t anomalies() const { return this->anomalies_; }
  uint32_t defrost_samples() const { return this->defrost_samples_; }

  // Operating windows count towards b only while their mean output is inside
  // this band (default MIN_OUTPUT..1). The replay narrows it to compare b
  // learned at full and at partial output.
  void set_output_band(float low, float high) {
    this->band_low_ = low;
    this->band_high_ = high;
  }

  ModelUpdate update(const ModelSample &sample) {
    ModelUpdate result;
    if (!std::isfinite(sample.room)) {
      this->reset_window_();
      this->has_previous_ = false;
      return result;
    }

    // A gap, a start or stop, a mode change or a defrost: the room is reacting
    // to something else for a while.
    const bool joined = this->has_previous_ && sample.now_ms - this->previous_ms_ <= MAX_SAMPLE_GAP_MS;
    if (sample.defrosting) {
      this->defrost_samples_++;
      this->transition_ms_ = sample.now_ms;
      this->reset_window_();
      this->dead_pending_ = false;
      this->remember_(sample);
      return result;
    }
    if (!joined || sample.operating != this->previous_operating_ || sample.heating != this->previous_heating_) {
      this->transition_ms_ = sample.now_ms;
      this->reset_window_();
    }

    this->track_dead_time_(sample, joined, result);

    const uint32_t settle_ms = static_cast<uint32_t>(this->dead_time_s(sample.heating) * 1000.0f);
    const bool settled = sample.now_ms - this->transition_ms_ >= settle_ms;
    const Regime regime = settled ? regime_of_(sample) : Regime::NONE;
    if (regime == Regime::NONE) {
      this->reset_window_();
    } else if (this->window_regime_ != regime) {
      this->start_window_(sample, regime);
    } else {
      const uint32_t dt = sample.now_ms - this->window_last_ms_;
      this->window_ms_ += dt;
      this->window_output_ += sample.output * dt;
      if (std::isfinite(sample.outdoor)) {
        this->window_gap_ += (sample.room - sample.outdoor) * dt;
      } else {
        this->window_outdoor_known_ = false;
      }
      this->window_last_ms_ = sample.now_ms;
      this->window_last_room_ = sample.room;
      if (this->window_regime_ != Regime::IDLE && sample.now_ms - this->check_start_ms_ >= CHECK_MS) {
        if (this->stretch_wrong_way_(sample)) {
          this->anomalies_++;
          result.anomaly = true;
          this->transition_ms_ = sample.now_ms;
          this->reset_window_();
          this->remember_(sample);
          return result;
        }
        this->check_start_ms_ = sample.now_ms;
        this->check_start_room_ = sample.room;
      }
      if (this->window_ms_ >= WINDOW_MS) {
        this->close_window_(sample.heating, result);
        this->start_window_(sample, regime);
      }
    }

    this->remember_(sample);
    return result;
  }

 private:
  enum class Regime : uint8_t { NONE, IDLE, HEAT, COOL };

  Regime regime_of_(const ModelSample &sample) const {
    if (!sample.operating) return Regime::IDLE;
    if (sample.output < MIN_OUTPUT) return Regime::NONE;
    return sample.heating ? Regime::HEAT : Regime::COOL;
  }

  void reset_window_() { this->window_regime_ = Regime::NONE; }

  void remember_(const ModelSample &sample) {
    this->has_previous_ = true;
    this->previous_ms_ = sample.now_ms;
    this->previous_operating_ = sample.operating;
    this->previous_heating_ = sample.heating;
  }

  // Whether the room moved the wrong way since check_start_ by more than CHECK_MARGIN beyond the drift.
  bool stretch_wrong_way_(const ModelSample &sample) const {
    const float seconds = (sample.now_ms - this->check_start_ms_) / 1000.0f;
    const float change = (sample.room - this->check_start_room_) / seconds;
    const float drift = std::isfinite(sample.outdoor) ? -this->state_.loss * (sample.room - sample.outdoor) : 0.0f;
    return sample.heating ? change < drift - CHECK_MARGIN : change > drift + CHECK_MARGIN;
  }

  void start_window_(const ModelSample &sample, Regime regime) {
    this->window_regime_ = regime;
    this->window_ms_ = 0;
    this->window_output_ = 0.0f;
    this->window_gap_ = 0.0f;
    this->window_outdoor_known_ = std::isfinite(sample.outdoor);
    this->window_last_ms_ = sample.now_ms;
    this->window_start_room_ = sample.room;
    this->window_last_room_ = sample.room;
    this->check_start_ms_ = sample.now_ms;
    this->check_start_room_ = sample.room;
  }

  void close_window_(bool heating, ModelUpdate &result) {
    const float seconds = this->window_ms_ / 1000.0f;
    const float change = (this->window_last_room_ - this->window_start_room_) / seconds;  // C/s
    const float gap = this->window_outdoor_known_ ? this->window_gap_ / this->window_ms_ : NAN;

    if (this->window_regime_ == Regime::IDLE) {
      if (std::isfinite(gap) && std::fabs(gap) >= MIN_LOSS_GAP) {
        const float measured = clamp_loss(-change / gap);
        this->state_.loss += LOSS_SMOOTHING * (measured - this->state_.loss);
        this->loss_n_++;
        result.loss = true;
      }
      return;
    }

    // Without an outdoor temperature the drift is unknown and left out.
    const float drift = std::isfinite(gap) ? -this->state_.loss * gap : 0.0f;
    const bool wrong_way = heating ? change < drift - ANOMALY_MARGIN : change > drift + ANOMALY_MARGIN;
    if (wrong_way) {
      this->anomalies_++;
      result.anomaly = true;
      return;
    }
    const float output = this->window_output_ / this->window_ms_;
    if (output < this->band_low_ || output > this->band_high_) {
      return;
    }
    const float measured = clamp_rate((heating ? change - drift : drift - change) / output);
    float &rate = heating ? this->state_.heat_rate : this->state_.cool_rate;
    rate += SMOOTHING * (measured - rate);
    (heating ? this->heat_n_ : this->cool_n_)++;
    result.rate = true;
  }

  // From a start, the time until the room turns the right way (see the top).
  void track_dead_time_(const ModelSample &sample, bool joined, ModelUpdate &result) {
    // How far the room is the right way of the start: positive is warmer while
    // heating, cooler while cooling.
    const float ahead = sample.heating ? sample.room : -sample.room;
    const bool started = sample.operating && !(joined && this->previous_operating_);
    if (started) {
      this->dead_pending_ = joined;  // after a gap the start time is unknown
      this->dead_start_ms_ = sample.now_ms;
      this->dead_extreme_ = ahead;
      this->dead_extreme_first_ms_ = sample.now_ms;
      this->dead_extreme_last_ms_ = sample.now_ms;
      return;
    }
    if (!this->dead_pending_) {
      return;
    }
    if (!joined || !sample.operating || sample.heating != this->previous_heating_ ||
        (sample.now_ms - this->dead_start_ms_) / 1000.0f > MAX_DEAD_S) {
      this->dead_pending_ = false;  // stopped, or never turned: no measurement
      return;
    }
    if (ahead < this->dead_extreme_ - DEAD_TIME_SAME) {
      // Still moving the wrong way: a new extreme.
      this->dead_extreme_ = ahead;
      this->dead_extreme_first_ms_ = sample.now_ms;
      this->dead_extreme_last_ms_ = sample.now_ms;
    } else if (ahead <= this->dead_extreme_ + DEAD_TIME_SAME) {
      this->dead_extreme_last_ms_ = sample.now_ms;  // still at the extreme
    } else if (ahead >= this->dead_extreme_ + DEAD_TIME_RISE) {
      const uint32_t turn_ms = this->dead_extreme_first_ms_ +
                               (this->dead_extreme_last_ms_ - this->dead_extreme_first_ms_) / 2;
      const float measured = clamp_dead((turn_ms - this->dead_start_ms_) / 1000.0f);
      float &dead = sample.heating ? this->state_.heat_dead_s : this->state_.cool_dead_s;
      dead = clamp_dead(dead + SMOOTHING * (measured - dead));
      (sample.heating ? this->heat_dead_n_ : this->cool_dead_n_)++;
      this->dead_pending_ = false;
      result.dead_time = true;
    }
  }

  ModelState state_{DEFAULT_LOSS, DEFAULT_RATE, DEFAULT_RATE, 300.0f, 300.0f};
  float band_low_{MIN_OUTPUT};
  float band_high_{1.0f};

  uint32_t heat_n_{0};
  uint32_t cool_n_{0};
  uint32_t loss_n_{0};
  uint32_t heat_dead_n_{0};
  uint32_t cool_dead_n_{0};
  uint32_t anomalies_{0};
  uint32_t defrost_samples_{0};

  bool has_previous_{false};
  uint32_t previous_ms_{0};
  bool previous_operating_{false};
  bool previous_heating_{true};
  uint32_t transition_ms_{0};

  Regime window_regime_{Regime::NONE};
  uint32_t window_ms_{0};
  uint32_t window_last_ms_{0};
  float window_output_{0.0f};  // output * ms
  float window_gap_{0.0f};     // (room - outdoor) * ms
  bool window_outdoor_known_{false};
  float window_start_room_{0.0f};
  float window_last_room_{0.0f};
  uint32_t check_start_ms_{0};
  float check_start_room_{0.0f};

  bool dead_pending_{false};
  uint32_t dead_start_ms_{0};
  float dead_extreme_{0.0f};  // furthest the wrong way since the start (see `ahead`)
  uint32_t dead_extreme_first_ms_{0};
  uint32_t dead_extreme_last_ms_{0};
};

}  // namespace pid
}  // namespace workflow
