#pragma once

#include <cmath>
#include <cstdint>

// Explicit start/stop control for units whose own thermostat has only three
// levels. No ESPHome dependency, so it can be tested natively
// (tests/test_start_controller.cpp). See docs/start-control-design.md.
//
// A Mitsubishi head compares its setpoint with the exact remote temperature it
// is sent (the room temperature R), although it reports that temperature back
// rounded to its 0.5 C step (the reported temperature):
//
//   setpoint >= R                  starts within about 20 s
//   reported - 0.5                 keeps running; a stopped unit restarts on its
//                                  own once its anti-short-cycle timer allows
//   reported - 1.0                 stays off; a running unit stops
//
// The PID's output O is the setpoint it would send if the unit took any value.
// Each update, O is compared with R, the way the unit compares a setpoint with
// R, and one of three setpoint levels is sent in 0.5 C steps:
//
//   O >= R              start level    the 0.5 step at or above R
//   R - 0.5 <= O < R    running level  reported - 0.5
//   O < R - 0.5         off level      reported - 1.0
//
// A level is left for a lower one only once O is level_deadband past the
// boundary, so noise on R doesn't flip it. The states (START, RUN, IDLE) track
// what the unit does with the level it is sent; guards (minimum cycle and run,
// heat from other heads, hysteresis) can hold a lower level than the PID asks.
//
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

// The setpoint level the PID's output asks for.
enum class Level : uint8_t { OFF = 0, KEEP = 1, START = 2 };

inline const char *level_to_string(Level level) {
  switch (level) {
    case Level::OFF:
      return "off";
    case Level::KEEP:
      return "running";
    case Level::START:
      return "start";
  }
  return "unknown";
}

enum class PowerRequest : uint8_t { NONE, TURN_ON, TURN_OFF };

enum class Transition : uint8_t {
  NONE,
  STARTED,            // IDLE -> START: the PID's output reached the room temperature
  RUNNING,            // START -> RUN: the unit has reported running for run_confirm_ms
  STOPPED_BY_UNIT,    // RUN -> IDLE: the unit stopped by itself
  STOPPED_PID,        // START/RUN -> IDLE: the PID's output fell to the off level
  STOPPED_SHARED,     // START/RUN -> IDLE: heat started arriving from other heads
  STOPPED_HOLD,       // START/RUN -> IDLE: past the off threshold, hysteresis is holding
  START_FAULT,        // START: the unit has not started within start_fault_ms (logged once)
  UNREQUESTED_START,  // IDLE -> RUN: the unit started without a START
  PARK,               // IDLE: shared heat past the off threshold; switching the device off
  PARKED,             // -> PARKED: the device is off (parked, or cut by hysteresis)
  UNPARK,             // PARKED: the room is past the on threshold; switching the device on
  UNPARKED,           // PARKED -> IDLE: the device is on again
  DEFERRED,           // IDLE: a start is held off; heat from other heads is expected soon
  STOPPED_EXPECTED,   // RUN -> IDLE: heat from other heads is expected within expected_stop_s
  CATCH,              // PARKED: below target and heat from other heads is expected or arriving; switching on
};

inline const char *transition_to_string(Transition transition) {
  switch (transition) {
    case Transition::NONE:
      return "none";
    case Transition::STARTED:
      return "start: the PID's output reached the room temperature";
    case Transition::RUNNING:
      return "running";
    case Transition::STOPPED_BY_UNIT:
      return "stopped by the unit";
    case Transition::STOPPED_PID:
      return "stopped: the PID's output fell to the off level";
    case Transition::STOPPED_SHARED:
      return "stopped: heat is arriving from other heads";
    case Transition::STOPPED_HOLD:
      return "stopped: past the off threshold";
    case Transition::START_FAULT:
      return "the unit has not started; still asking";
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
    case Transition::DEFERRED:
      return "start deferred: heat from other heads is expected soon";
    case Transition::STOPPED_EXPECTED:
      return "stopped: heat from other heads is expected soon";
    case Transition::CATCH:
      return "switching on to take heat from other heads";
  }
  return "unknown";
}

struct StartConfig {
  // How far the PID's output may be short of the room (C, in the direction of
  // output) and still hold the running level; past it, the off level.
  float stop_band{0.5f};
  // A level is left for a lower one only once the PID's output is this far
  // past the boundary (C), so noise on the room reading doesn't flip it.
  float level_deadband{0.05f};
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
  // A start the unit hasn't followed in this long is logged (once). The start
  // level is kept: the PID still asks for heat.
  uint32_t start_fault_ms{900000};
  // Heat from other heads, forecast by the supervisor (another head is
  // predicted to start). A forecast this close counts:
  uint32_t expect_window_s{300};
  // A start is deferred while heat is expected, unless the room is this far
  // short of the target (C) or the deferral has lasted defer_max_ms.
  float defer_floor{0.5f};
  uint32_t defer_max_ms{360000};
  // A run past min_run_ms stops when heat is expected within this.
  uint32_t expected_stop_s{120};
  // A parked head is switched on to take heat from other heads (expected or
  // arriving) once the room is this far short of the target (C).
  float catch_below{0.1f};
  // A start becomes a run once the unit has reported running this long without
  // a break. Right after power-on a unit can report running for ~20 s and stop;
  // until confirmed the start level is kept, so the unit is still asked to
  // start.
  uint32_t run_confirm_ms{60000};
  // After a stop is commanded, the unit may report running for a while longer.
  uint32_t stop_grace_ms{60000};
  // A run is not stopped by the PID before this; the room only starts to
  // respond after the unit's warm-up.
  uint32_t min_run_ms{120000};
};

struct StartInput {
  uint32_t now_ms{0};
  bool heating{true};
  bool powered{false};    // the device is commanded on
  bool operating{false};  // the unit reports its compressor running
  float device_view{NAN};  // the temperature the unit reports (its step); NaN when unknown
  float room{NAN};         // the room temperature the controller regulates, sent to the unit
  float target{NAN};
  float output{NAN};        // the PID's output: the setpoint it asks for
  bool shared_heat{false};  // heat (cooling) arriving from other heads
  // Seconds until heat (cooling) from other heads is expected (0 once due);
  // NaN when nothing is forecast.
  float shared_expected_s{NAN};
  // Hysteresis is holding: the room is past its off threshold and it is
  // waiting out off_delay before switching the device off.
  bool hold{false};
};

struct StartDecision {
  HeadState state{HeadState::IDLE};
  // The level the PID's output asks for (before any guard).
  Level level{Level::OFF};
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

  HeadState state() const { return this->state_; }
  Level level() const { return this->level_; }
  // Time since the last stop, or since the device came on; UINT32_MAX if never.
  uint32_t since_stop_ms(uint32_t now_ms) const {
    return this->has_stop_ ? now_ms - this->last_stop_ms_ : UINT32_MAX;
  }

  // Continuous mode only (start_engagement.h): take over a head the regular
  // PID path has been driving. The state is adopted from the device (off:
  // PARKED, running: RUN, else IDLE). The last stop and power change seen
  // while not engaged are passed in, so the minimum cycle still holds across
  // the change; without them the ones this controller saw last are kept.
  void resync(const StartInput &in, bool power_changed, uint32_t power_change_ms, bool stopped, uint32_t stop_ms) {
    this->initialized_ = true;
    this->heating_ = in.heating;
    this->level_ = Level::OFF;
    this->deferring_ = false;
    this->fault_reported_ = false;
    this->operating_since_valid_ = false;
    this->stop_cmd_ms_ = in.now_ms - this->config_.stop_grace_ms;
    if (!in.powered) {
      this->state_ = HeadState::PARKED;
    } else if (in.operating) {
      this->state_ = HeadState::RUN;
      this->start_ms_ = in.now_ms;
    } else {
      this->state_ = HeadState::IDLE;
    }
    if (power_changed) {
      this->note_power_change_(power_change_ms);
    }
    if (stopped) {
      this->note_stop_(stop_ms);
    }
  }

  StartDecision update(const StartInput &in) {
    StartDecision out;
    const float sign = in.heating ? 1.0f : -1.0f;
    const float view = this->view_(in);

    if (!this->initialized_ || in.heating != this->heating_) {
      this->heating_ = in.heating;
      this->initialized_ = true;
      this->has_power_change_ = false;
      this->has_stop_ = false;
      this->level_ = Level::OFF;
      this->stop_cmd_ms_ = in.now_ms - this->config_.stop_grace_ms;
      if (!in.powered) {
        this->state_ = HeadState::PARKED;
      } else if (in.operating) {
        this->state_ = HeadState::RUN;
        this->start_ms_ = in.now_ms;
      } else {
        this->state_ = HeadState::IDLE;
      }
    }

    this->level_ = this->next_level_(in, sign);
    out.level = this->level_;

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

    // Heat from other heads is forecast within the window.
    const bool expected = std::isfinite(in.shared_expected_s) &&
                          in.shared_expected_s <= static_cast<float>(this->config_.expect_window_s);
    if (this->state_ != HeadState::IDLE) {
      this->deferring_ = false;
    }

    switch (this->state_) {
      case HeadState::PARKED: {
        const bool want_on = known && -past > this->config_.on_threshold;
        // Below target with heat from other heads on its way: switch on so the
        // head's fan brings it in, rather than starting a run of its own later.
        const bool catch_heat = known && (expected || in.shared_heat) && -past > this->config_.catch_below;
        if ((want_on || catch_heat) && this->power_change_allowed_(in.now_ms)) {
          out.power = PowerRequest::TURN_ON;
          out.transition = want_on ? Transition::UNPARK : Transition::CATCH;
        }
        break;
      }
      case HeadState::IDLE: {
        if (in.operating &&
            static_cast<uint32_t>(in.now_ms - this->stop_cmd_ms_) >= this->config_.stop_grace_ms) {
          this->state_ = HeadState::RUN;
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
        bool start = this->level_ == Level::START && !in.shared_heat && !in.hold && std::isfinite(view) &&
                     this->since_stop_ms(in.now_ms) >= this->config_.min_cycle_ms;
        // Heat from other heads is due: wait for it rather than start a run
        // that would stack on top of it. Not past the floor, and not for long.
        // A deferral lasts while a start is wanted and the heat is expected;
        // once either goes (the heat came, or the forecast was dropped) it ends.
        const bool defer = start && expected && known && -past < this->config_.defer_floor;
        if (!defer) {
          this->deferring_ = false;
        }
        if (defer) {
          if (!this->deferring_) {
            this->deferring_ = true;
            this->defer_since_ms_ = in.now_ms;
            out.transition = Transition::DEFERRED;
          }
          if (static_cast<uint32_t>(in.now_ms - this->defer_since_ms_) < this->config_.defer_max_ms) {
            start = false;
          }
        }
        if (start) {
          this->deferring_ = false;
          this->state_ = HeadState::START;
          this->start_ms_ = in.now_ms;
          this->operating_since_valid_ = false;
          this->fault_reported_ = false;
          out.transition = Transition::STARTED;
        }
        break;
      }
      case HeadState::START: {
        const bool ran_long_enough =
            static_cast<uint32_t>(in.now_ms - this->start_ms_) >= this->config_.min_run_ms;
        if (in.hold) {
          this->stop_(in.now_ms, true);
          out.transition = Transition::STOPPED_HOLD;
        } else if (in.shared_heat) {
          // Heat from other heads arrived while the unit was still starting.
          this->stop_(in.now_ms, true);
          out.transition = Transition::STOPPED_SHARED;
        } else if (in.operating) {
          // Running, but not yet confirmed: the level the PID asks for stays.
          if (this->level_ == Level::OFF && ran_long_enough) {
            this->stop_(in.now_ms, true);
            out.transition = Transition::STOPPED_PID;
            break;
          }
          if (!this->operating_since_valid_) {
            this->operating_since_valid_ = true;
            this->operating_since_ms_ = in.now_ms;
          }
          if (static_cast<uint32_t>(in.now_ms - this->operating_since_ms_) >= this->config_.run_confirm_ms) {
            this->state_ = HeadState::RUN;
            out.transition = Transition::RUNNING;
          }
        } else {
          this->operating_since_valid_ = false;
          if (this->level_ != Level::START) {
            // The PID no longer asks for a start, and the unit never ran: no
            // stop to count, so the minimum cycle doesn't apply.
            this->state_ = HeadState::IDLE;
            out.transition = Transition::STOPPED_PID;
          } else if (!this->fault_reported_ &&
                     static_cast<uint32_t>(in.now_ms - this->start_ms_) >= this->config_.start_fault_ms) {
            this->fault_reported_ = true;
            out.transition = Transition::START_FAULT;
          }
        }
        break;
      }
      case HeadState::RUN: {
        if (!in.operating) {
          this->state_ = HeadState::IDLE;
          this->note_stop_(in.now_ms);
          out.transition = Transition::STOPPED_BY_UNIT;
          break;
        }
        if (in.hold) {
          this->stop_(in.now_ms, true);
          out.transition = Transition::STOPPED_HOLD;
          break;
        }
        const bool ran_long_enough =
            static_cast<uint32_t>(in.now_ms - this->start_ms_) >= this->config_.min_run_ms;
        const bool expected_soon =
            std::isfinite(in.shared_expected_s) &&
            in.shared_expected_s <= static_cast<float>(this->config_.expected_stop_s);
        if (ran_long_enough && this->level_ == Level::OFF) {
          this->stop_(in.now_ms, true);
          out.transition = Transition::STOPPED_PID;
        } else if (ran_long_enough && expected_soon) {
          this->stop_(in.now_ms, true);
          out.transition = Transition::STOPPED_EXPECTED;
        } else if (in.shared_heat) {
          this->stop_(in.now_ms, true);
          out.transition = Transition::STOPPED_SHARED;
        }
        break;
      }
    }

    out.state = this->state_;
    out.setpoint = this->setpoint_(in, view, sign, expected);
    // The integral follows the room through every cycle, so a room that
    // averages past the target moves the start and stop points; it is frozen
    // only while the device is off.
    out.freeze_integral = this->state_ == HeadState::PARKED;
    return out;
  }

 private:
  // The level the PID's output asks for. A level is left for a lower one only
  // once the output is level_deadband past the boundary.
  Level next_level_(const StartInput &in, float sign) const {
    if (!std::isfinite(in.output) || !std::isfinite(in.room)) {
      return Level::OFF;
    }
    const float d = sign * (in.output - in.room);
    const float start_at = this->level_ == Level::START ? -this->config_.level_deadband : 0.0f;
    if (d >= start_at) {
      return Level::START;
    }
    const float keep_at = this->level_ == Level::OFF ? -this->config_.stop_band
                                                     : -this->config_.stop_band - this->config_.level_deadband;
    return d >= keep_at ? Level::KEEP : Level::OFF;
  }

  // The temperature the unit reports; falls back to the room rounded to the step.
  float view_(const StartInput &in) const {
    if (std::isfinite(in.device_view)) {
      return in.device_view;
    }
    if (std::isfinite(in.room) && this->config_.step > 0.0f) {
      return std::round(in.room / this->config_.step) * this->config_.step;
    }
    return NAN;
  }

  float step_() const { return this->config_.step > 0.0f ? this->config_.step : 0.5f; }

  // The setpoint at or past the room in the direction of output: the unit starts on it.
  float start_level_(const StartInput &in, float view, float sign) const {
    if (!std::isfinite(in.room)) {
      return view;
    }
    const float step = this->step_();
    constexpr float GRID_TOLERANCE = 0.001f;  // in steps
    const float steps = in.room / step;
    return (sign > 0.0f ? std::ceil(steps - GRID_TOLERANCE) : std::floor(steps + GRID_TOLERANCE)) * step;
  }

  float setpoint_(const StartInput &in, float view, float sign, bool expected) const {
    const float step = this->step_();
    const float keep = view - sign * step;
    const float off = view - sign * 2.0f * step;
    switch (this->state_) {
      case HeadState::START:
      case HeadState::RUN:
        // Within min_run the off level is not sent; the running level is held.
        return this->level_ == Level::START ? this->start_level_(in, view, sign) : keep;
      case HeadState::IDLE: {
        // The running level lets a stopped unit restart on its own timer, so
        // only while the PID asks for it and nothing holds the head back.
        const bool keep_ok = this->level_ != Level::OFF && !in.shared_heat && !in.hold && !expected &&
                             static_cast<uint32_t>(in.now_ms - this->stop_cmd_ms_) >= this->config_.stop_grace_ms;
        return keep_ok ? keep : off;
      }
      case HeadState::PARKED:
        return off;
    }
    return NAN;
  }

  void stop_(uint32_t now_ms, bool commanded) {
    this->state_ = HeadState::IDLE;
    this->note_stop_(now_ms);
    if (commanded) {
      this->stop_cmd_ms_ = now_ms;
    }
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
  Level level_{Level::OFF};
  uint32_t start_ms_{0};
  bool fault_reported_{false};
  // While in START: when the unit's current spell of reporting running began.
  bool operating_since_valid_{false};
  uint32_t operating_since_ms_{0};
  // A start held off for expected heat from other heads, and since when.
  bool deferring_{false};
  uint32_t defer_since_ms_{0};
  bool has_stop_{false};
  uint32_t last_stop_ms_{0};
  uint32_t stop_cmd_ms_{0};
  bool has_power_change_{false};
  uint32_t last_power_change_ms_{0};
};

}  // namespace start
}  // namespace workflow
