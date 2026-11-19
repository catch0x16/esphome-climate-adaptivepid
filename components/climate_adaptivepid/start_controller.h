#pragma once

#include <cmath>
#include <cstdint>

// Explicit start/stop control for units whose own thermostat has only three
// levels. No ESPHome dependency, so it can be tested natively
// (tests/test_start_controller.cpp). See docs/start-control-design.md.
//
// A Mitsubishi head regulates on its own view of the room (the remote
// temperature, rounded to its 0.5 C step). Against that view:
//
//   setpoint - view >= 0     starts within seconds
//   setpoint - view == -0.5  keeps running; a stopped unit restarts on its own
//                            once its anti-short-cycle timer allows
//   setpoint - view <= -1.0  stays off; a running unit stops
//
// Rounding a continuous PID output onto that grid turns small corrections into
// full starts at whatever deficit a rounding boundary happens to fall. Here the
// PID only supplies demand. The state machine decides when to start, and sends
// one of three setpoints relative to the unit's view:
//
//   IDLE    view - 2 steps    unit off, cannot restart on its timer
//   START   view              start now
//   RUN     view at start - 1 step   lowest demand; the unit stops by itself
//                                    as soon as its view moves one step
//   PARKED  device switched off (a powered idle head still blows heat that
//           leaks from a shared outdoor unit into the room; only switching it
//           off stops that)
//
// Power follows the hysteresis band: a head is parked once the room is
// off_threshold past the target while heat arrives from other heads (without
// shared heat the hysteresis step cuts it, after its off_delay), and switched
// back on, idle, once the room is on_threshold short of the target.
//
// Everything is mirrored when cooling.
namespace workflow {
namespace start {

enum class HeadState : uint8_t { IDLE = 0, START = 1, RUN = 2, PARKED = 3 };

inline const char *head_state_to_string(HeadState state) {
  switch (state) {
    case HeadState::IDLE:
      return "IDLE";
    case HeadState::START:
      return "START";
    case HeadState::RUN:
      return "RUN";
    case HeadState::PARKED:
      return "PARKED";
  }
  return "UNKNOWN";
}

enum class PowerRequest : uint8_t { NONE, TURN_ON, TURN_OFF };

enum class Transition : uint8_t {
  NONE,
  STARTED,            // IDLE -> START: demand reached the start threshold
  RUNNING,            // START -> RUN: the unit has reported running for run_confirm_ms
  START_TIMED_OUT,    // START -> IDLE: the unit never started
  STOPPED_BY_UNIT,    // RUN -> IDLE: the unit stopped by itself
  STOPPED_EARLY,      // RUN -> IDLE: the predicted peak reached the target
  STOPPED_SHARED,     // START/RUN -> IDLE: heat started arriving from other heads
  STOPPED_HOLD,       // START/RUN -> IDLE: past the off threshold, hysteresis is holding
  UNREQUESTED_START,  // IDLE -> RUN: the unit started without a START
  PARK,               // IDLE: shared heat past the off threshold; switching the device off
  PARKED,             // -> PARKED: the device is off (parked, or cut by hysteresis)
  UNPARK,             // PARKED: the room is past the on threshold; switching the device on
  UNPARKED,           // PARKED -> IDLE: the device is on again
};

inline const char *transition_to_string(Transition transition) {
  switch (transition) {
    case Transition::NONE:
      return "none";
    case Transition::STARTED:
      return "start: demand reached the start threshold";
    case Transition::RUNNING:
      return "running";
    case Transition::START_TIMED_OUT:
      return "start timed out: the unit did not start";
    case Transition::STOPPED_BY_UNIT:
      return "stopped by the unit";
    case Transition::STOPPED_EARLY:
      return "stopped early: the predicted peak reached the target";
    case Transition::STOPPED_SHARED:
      return "stopped: heat is arriving from other heads";
    case Transition::STOPPED_HOLD:
      return "stopped: past the off threshold";
    case Transition::UNREQUESTED_START:
      return "the unit started without a start decision";
    case Transition::PARK:
      return "park: switching the device off";
    case Transition::PARKED:
      return "parked: the device is off";
    case Transition::UNPARK:
      return "unpark: switching the device on";
    case Transition::UNPARKED:
      return "unparked: the device is on";
  }
  return "unknown";
}

struct StartConfig {
  // PID demand (output - target, C; mirrored when cooling) that starts a run.
  float start_threshold{0.5f};
  // Hysteresis band (C past the target in the direction of output): a parked
  // head is switched back on once the room is on_threshold short of the
  // target, and parked while heat arrives from other heads once the room is
  // off_threshold past it.
  float on_threshold{0.25f};
  float off_threshold{0.5f};
  // The device's setpoint step.
  float step{0.5f};
  // Between a stop and the next start, and between power changes.
  uint32_t min_cycle_ms{60000};
  // A START the unit doesn't follow within this goes back to IDLE. The units
  // can take minutes to act on a start (up to 4 min seen), so it is held well
  // past that; a shorter hold gives up and retries from the stay-off level.
  uint32_t start_timeout_ms{300000};
  // A start becomes a run once the unit has reported running this long without
  // a break. Right after power-on a unit can report running for ~20 s and stop;
  // until confirmed the start setpoint is kept, so the unit is still asked to
  // start.
  uint32_t run_confirm_ms{60000};
  // After a stop is commanded, the unit may report running for a while longer.
  uint32_t stop_grace_ms{60000};
  // A run is not stopped early before this; the room only starts to respond
  // after the unit's warm-up.
  uint32_t min_run_ms{120000};
};

struct StartInput {
  uint32_t now_ms{0};
  bool heating{true};
  bool powered{false};    // the device is commanded on
  bool operating{false};  // the unit reports its compressor running
  float device_view{NAN};  // the unit's view of the room; NaN when unknown
  float room{NAN};         // the room temperature the controller regulates
  float target{NAN};
  float demand{0.0f};       // PID output - target, mirrored when cooling
  bool room_rising{false};  // the room is moving the way output would move it
  bool shared_heat{false};  // heat (cooling) arriving from other heads
  float coast{0.0f};        // expected further move after a stop, >= 0
  // Hysteresis is holding: the room is past its off threshold and it is
  // waiting out off_delay before switching the device off.
  bool hold{false};
};

struct StartDecision {
  HeadState state{HeadState::IDLE};
  // The setpoint for the device: the level while powered, and the setpoint to
  // switch on with while parked. NaN when the room temperature is unknown.
  float setpoint{NAN};
  PowerRequest power{PowerRequest::NONE};
  bool freeze_integral{false};
  Transition transition{Transition::NONE};
};

class StartController {
 public:
  void configure(const StartConfig &config) { this->config_ = config; }
  const StartConfig &config() const { return this->config_; }
  void set_start_threshold(float value) { this->config_.start_threshold = value; }

  HeadState state() const { return this->state_; }
  // Time since the last stop, or since the device came on; UINT32_MAX if never.
  uint32_t since_stop_ms(uint32_t now_ms) const {
    return this->has_stop_ ? now_ms - this->last_stop_ms_ : UINT32_MAX;
  }
  float start_view() const { return this->start_view_; }

  StartDecision update(const StartInput &in) {
    StartDecision out;
    const float sign = in.heating ? 1.0f : -1.0f;
    const float view = this->view_(in);

    if (!this->initialized_ || in.heating != this->heating_) {
      this->heating_ = in.heating;
      this->initialized_ = true;
      this->has_power_change_ = false;
      this->has_stop_ = false;
      this->stop_cmd_ms_ = in.now_ms - this->config_.stop_grace_ms;
      if (!in.powered) {
        this->state_ = HeadState::PARKED;
      } else if (in.operating) {
        this->state_ = HeadState::RUN;
        this->start_view_ = view;
        this->start_ms_ = in.now_ms;
      } else {
        this->state_ = HeadState::IDLE;
      }
    }

    // Power changes made elsewhere (hysteresis cut, a parked head switched on).
    if (!in.powered && this->state_ != HeadState::PARKED) {
      if (this->state_ == HeadState::RUN || this->state_ == HeadState::START) {
        this->note_stop_(in.now_ms);
      }
      this->state_ = HeadState::PARKED;
      this->note_power_change_(in.now_ms);
      out.transition = Transition::PARKED;
    } else if (in.powered && this->state_ == HeadState::PARKED) {
      this->state_ = HeadState::IDLE;
      this->note_power_change_(in.now_ms);
      out.transition = Transition::UNPARKED;
    }

    // How far the room is past the target in the direction of output.
    const bool known = std::isfinite(in.room) && std::isfinite(in.target);
    const float past = known ? sign * (in.room - in.target) : NAN;

    switch (this->state_) {
      case HeadState::PARKED: {
        const bool want_on = known && -past > this->config_.on_threshold;
        if (want_on && this->power_change_allowed_(in.now_ms)) {
          out.power = PowerRequest::TURN_ON;
          out.transition = Transition::UNPARK;
        }
        break;
      }
      case HeadState::IDLE: {
        if (in.operating &&
            static_cast<uint32_t>(in.now_ms - this->stop_cmd_ms_) >= this->config_.stop_grace_ms) {
          this->state_ = HeadState::RUN;
          this->start_view_ = view;
          this->start_ms_ = in.now_ms;
          out.transition = Transition::UNREQUESTED_START;
          break;
        }
        // Heat from other heads won't stop while the head is powered: cut at
        // the off threshold at once rather than waiting out off_delay.
        if (in.shared_heat && known && past > this->config_.off_threshold) {
          if (this->power_change_allowed_(in.now_ms)) {
            out.power = PowerRequest::TURN_OFF;
            out.transition = Transition::PARK;
          }
          break;
        }
        const bool start = in.demand >= this->config_.start_threshold && !in.shared_heat && !in.room_rising &&
                           !in.hold && std::isfinite(view) &&
                           this->since_stop_ms(in.now_ms) >= this->config_.min_cycle_ms;
        if (start) {
          this->state_ = HeadState::START;
          this->start_view_ = view;
          this->start_ms_ = in.now_ms;
          this->operating_since_valid_ = false;
          out.transition = Transition::STARTED;
        }
        break;
      }
      case HeadState::START:
        if (in.hold) {
          this->state_ = HeadState::IDLE;
          this->note_stop_(in.now_ms);
          this->stop_cmd_ms_ = in.now_ms;
          out.transition = Transition::STOPPED_HOLD;
        } else if (in.shared_heat) {
          // Heat from other heads arrived while the unit was still starting.
          this->state_ = HeadState::IDLE;
          this->note_stop_(in.now_ms);
          this->stop_cmd_ms_ = in.now_ms;
          out.transition = Transition::STOPPED_SHARED;
        } else if (in.operating) {
          // Running, but not yet confirmed: keep asking the unit to start.
          // The start doesn't time out while the unit reports running.
          if (!this->operating_since_valid_) {
            this->operating_since_valid_ = true;
            this->operating_since_ms_ = in.now_ms;
          }
          if (static_cast<uint32_t>(in.now_ms - this->operating_since_ms_) >= this->config_.run_confirm_ms) {
            this->state_ = HeadState::RUN;
            out.transition = Transition::RUNNING;
          }
        } else {
          // Not running (or a short spell ended): the start setpoint stays.
          this->operating_since_valid_ = false;
          if (static_cast<uint32_t>(in.now_ms - this->start_ms_) >= this->config_.start_timeout_ms) {
            this->state_ = HeadState::IDLE;
            this->note_stop_(in.now_ms);
            out.transition = Transition::START_TIMED_OUT;
          }
        }
        break;
      case HeadState::RUN: {
        if (!in.operating) {
          this->state_ = HeadState::IDLE;
          this->note_stop_(in.now_ms);
          out.transition = Transition::STOPPED_BY_UNIT;
          break;
        }
        if (in.hold) {
          this->state_ = HeadState::IDLE;
          this->note_stop_(in.now_ms);
          this->stop_cmd_ms_ = in.now_ms;
          out.transition = Transition::STOPPED_HOLD;
          break;
        }
        const bool ran_long_enough =
            static_cast<uint32_t>(in.now_ms - this->start_ms_) >= this->config_.min_run_ms;
        const bool peak_reaches_target = known && past + in.coast >= 0.0f;
        if (ran_long_enough && peak_reaches_target) {
          this->state_ = HeadState::IDLE;
          this->note_stop_(in.now_ms);
          this->stop_cmd_ms_ = in.now_ms;
          out.transition = Transition::STOPPED_EARLY;
        } else if (in.shared_heat) {
          this->state_ = HeadState::IDLE;
          this->note_stop_(in.now_ms);
          this->stop_cmd_ms_ = in.now_ms;
          out.transition = Transition::STOPPED_SHARED;
        }
        break;
      }
    }

    out.state = this->state_;
    out.setpoint = this->setpoint_(view, sign);
    // While the output is held below what the PID asks for, its integral must
    // not build up against it.
    out.freeze_integral = this->state_ != HeadState::IDLE;
    return out;
  }

 private:
  // The unit's view of the room; falls back to the room rounded to the step.
  float view_(const StartInput &in) const {
    if (std::isfinite(in.device_view)) {
      return in.device_view;
    }
    if (std::isfinite(in.room) && this->config_.step > 0.0f) {
      return std::round(in.room / this->config_.step) * this->config_.step;
    }
    return NAN;
  }

  float setpoint_(float view, float sign) const {
    const float step = this->config_.step > 0.0f ? this->config_.step : 0.5f;
    switch (this->state_) {
      case HeadState::START:
        return this->start_view_;
      case HeadState::RUN:
        return this->start_view_ - sign * step;
      case HeadState::IDLE:
      case HeadState::PARKED:
        return view - sign * 2.0f * step;
    }
    return NAN;
  }

  bool power_change_allowed_(uint32_t now_ms) const {
    return !this->has_power_change_ ||
           static_cast<uint32_t>(now_ms - this->last_power_change_ms_) >= this->config_.min_cycle_ms;
  }
  void note_power_change_(uint32_t now_ms) {
    this->has_power_change_ = true;
    this->last_power_change_ms_ = now_ms;
  }
  void note_stop_(uint32_t now_ms) {
    this->has_stop_ = true;
    this->last_stop_ms_ = now_ms;
  }

  StartConfig config_{};
  bool initialized_{false};
  bool heating_{true};
  HeadState state_{HeadState::IDLE};
  float start_view_{NAN};
  uint32_t start_ms_{0};
  // While in START: when the unit's current spell of reporting running began.
  bool operating_since_valid_{false};
  uint32_t operating_since_ms_{0};
  bool has_stop_{false};
  uint32_t last_stop_ms_{0};
  uint32_t stop_cmd_ms_{0};
  bool has_power_change_{false};
  uint32_t last_power_change_ms_{0};
};

}  // namespace start
}  // namespace workflow
