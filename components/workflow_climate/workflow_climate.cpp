#include "workflow_climate.h"

#include <algorithm>
#include <new>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "hysterisis_workflowstep.h"
#include "pid_workflowstep.h"

namespace esphome {
namespace workflow_climate {

static const char *const TAG = "workflow_climate";

using workflow::PowerEvent;

// Assign a float, reporting whether it changed. Two NaNs count as equal.
static bool assign_float(float &destination, float source) {
  if (destination == source || (std::isnan(destination) && std::isnan(source))) {
    return false;
  }
  destination = source;
  return true;
}

static void publish_binary(binary_sensor::BinarySensor *sensor, bool value) {
  if (sensor != nullptr && (!sensor->has_state() || sensor->state != value)) {
    sensor->publish_state(value);
  }
}

static void publish_value(sensor::Sensor *sensor, float value) {
  if (sensor != nullptr && std::isfinite(value) && (!sensor->has_state() || sensor->state != value)) {
    sensor->publish_state(value);
  }
}

void WorkflowClimate::set_inner_climate(climate::Climate *inner) {
  this->inner_ = inner;
  inner->add_on_state_callback([this](climate::Climate &) { this->on_inner_state_(); });
}

void WorkflowClimate::set_temperature_sensor(sensor::Sensor *sensor) {
  this->temperature_sensor_ = sensor;
  sensor->add_on_state_callback([this](float) {
    if (this->initialized_ && assign_float(this->current_temperature, this->room_temperature_())) {
      this->publish_state();
    }
  });
}

float WorkflowClimate::room_temperature_() const {
  if (this->temperature_sensor_ != nullptr && this->temperature_sensor_->has_state() &&
      std::isfinite(this->temperature_sensor_->state)) {
    return this->temperature_sensor_->state;
  }
  return this->inner_->current_temperature;
}

float WorkflowClimate::effective_min_temperature_() const {
  float value = this->traits_.get_visual_min_temperature();
#ifdef USE_CLIMATE_VISUAL_OVERRIDES
  if (std::isfinite(this->visual_min_temperature_override_)) {
    value = std::max(value, this->visual_min_temperature_override_);
  }
#endif
  return value;
}

float WorkflowClimate::effective_max_temperature_() const {
  float value = this->traits_.get_visual_max_temperature();
#ifdef USE_CLIMATE_VISUAL_OVERRIDES
  if (std::isfinite(this->visual_max_temperature_override_)) {
    value = std::min(value, this->visual_max_temperature_override_);
  }
#endif
  return value;
}

void WorkflowClimate::refresh_traits_() {
  // The inner climate's traits, including its own visual overrides. This
  // climate's `visual:` settings are applied on top by get_traits().
  this->traits_ = this->inner_->get_traits();
  this->set_supported_custom_fan_modes(this->traits_.get_supported_custom_fan_modes());
  this->set_supported_custom_presets(this->traits_.get_supported_custom_presets());
  if (this->temperature_sensor_ != nullptr) {
    this->traits_.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  }
  this->device_.set_limits(this->effective_min_temperature_(), this->effective_max_temperature_(),
                           this->traits_.get_visual_target_temperature_step());
}

float WorkflowClimate::stored_target_(climate::ClimateMode mode) const {
  return mode == climate::CLIMATE_MODE_COOL ? this->stored_targets_.cool : this->stored_targets_.heat;
}

void WorkflowClimate::store_target_(climate::ClimateMode mode, float value) {
  float &slot = mode == climate::CLIMATE_MODE_COOL ? this->stored_targets_.cool : this->stored_targets_.heat;
  if (slot == value) {
    return;
  }
  slot = value;
  this->targets_preference_.save(&this->stored_targets_);
}

void WorkflowClimate::load_targets_() {
  StoredTargets loaded{};
  if (this->targets_preference_.load(&loaded) && loaded.version == TARGETS_VERSION &&
      std::isfinite(loaded.heat) && std::isfinite(loaded.cool)) {
    this->stored_targets_.heat = this->device_.clamp_target(loaded.heat);
    this->stored_targets_.cool = this->device_.clamp_target(loaded.cool);
    this->targets_loaded_ = true;
  }
  this->stored_targets_.version = TARGETS_VERSION;
}

// Per hour for logs and sensors, from per second.
static float per_hour(float per_second) { return per_second * 3600.0f; }

void WorkflowClimate::load_tuning_() {
  if (this->pid_step_ == nullptr || !this->pid_step_->isAdaptive()) {
    return;
  }
  using workflow::pid::ModelState;
  using workflow::pid::Resolved;
  using workflow::pid::RoomModel;
  using workflow::pid::TuningSource;

  this->seeds_.loss = std::isfinite(this->initial_heat_loss_) ? RoomModel::clamp_loss(this->initial_heat_loss_)
                                                              : RoomModel::DEFAULT_LOSS;
  this->seeds_.heat_rate = workflow::pid::configured_seed(this->initial_heat_rate_, this->kp_, this->tuning_);
  this->seeds_.cool_rate = workflow::pid::configured_seed(this->initial_cool_rate_, this->kp_, this->tuning_);
  this->seeds_.heat_dead_s = RoomModel::clamp_dead(this->tuning_.dead_time_s);
  this->seeds_.cool_dead_s = this->seeds_.heat_dead_s;

  // What is in flash, and the seeds it was learned from. NaN: nothing usable,
  // so that value starts from its seed and the first save happens below.
  const ModelState none{NAN, NAN, NAN, NAN, NAN};
  this->stored_tuning_ = StoredTuning{TUNING_VERSION, none, none};
  ModelState stored = none;
  ModelState stored_seeds = none;
  StoredTuning loaded{};
  if (this->tuning_preference_.load(&loaded) && loaded.version == TUNING_VERSION) {
    this->stored_tuning_ = loaded;
    stored = loaded.model;
    stored_seeds = loaded.seeds;
  } else {
    // Earlier versions learned response rates only; the rest starts from its seed.
    StoredTuningV2 v2{};
    StoredTuningV1 v1{};
    if (this->make_entity_preference<StoredTuningV2>(TUNING_V2_PREFERENCE_TYPE).load(&v2) &&
        v2.version == TUNING_V2_VERSION) {
      stored.heat_rate = v2.heat_rate;
      stored.cool_rate = v2.cool_rate;
      stored_seeds.heat_rate = v2.seed_heat;
      stored_seeds.cool_rate = v2.seed_cool;
      ESP_LOGI(TAG, "Migrating learned response rates from storage v2");
    } else if (this->make_entity_preference<StoredTuningV1>(TUNING_V1_PREFERENCE_TYPE).load(&v1) &&
               v1.version == TUNING_V1_VERSION) {
      // v1 stored no seeds and always kept learned rates over the
      // configuration, so treat them as learned from today's seeds.
      stored.heat_rate = v1.heat_rate;
      stored.cool_rate = v1.cool_rate;
      stored_seeds.heat_rate = this->seeds_.heat_rate;
      stored_seeds.cool_rate = this->seeds_.cool_rate;
      ESP_LOGI(TAG, "Migrating learned response rates from storage v1");
    }
  }

  const Resolved loss = workflow::pid::resolve(stored.loss, stored_seeds.loss, this->seeds_.loss, 0.0f,
                                               RoomModel::MAX_LOSS);
  const Resolved heat = workflow::pid::resolve(stored.heat_rate, stored_seeds.heat_rate, this->seeds_.heat_rate);
  const Resolved cool = workflow::pid::resolve(stored.cool_rate, stored_seeds.cool_rate, this->seeds_.cool_rate);
  const Resolved heat_dead = workflow::pid::resolve(stored.heat_dead_s, stored_seeds.heat_dead_s,
                                                    this->seeds_.heat_dead_s, RoomModel::MIN_DEAD_S,
                                                    RoomModel::MAX_DEAD_S);
  const Resolved cool_dead = workflow::pid::resolve(stored.cool_dead_s, stored_seeds.cool_dead_s,
                                                    this->seeds_.cool_dead_s, RoomModel::MIN_DEAD_S,
                                                    RoomModel::MAX_DEAD_S);
  this->sources_ = ModelSources{loss.source, heat.source, cool.source, heat_dead.source, cool_dead.source};
  this->pid_step_->startModel(ModelState{loss.value, heat.value, cool.value, heat_dead.value, cool_dead.value},
                              heat.source == TuningSource::RESTORED, cool.source == TuningSource::RESTORED);
  this->log_model_("Room model");

  // Record the seeds in use, so the next boot compares against them.
  this->save_tuning_(millis(), true);
}

void WorkflowClimate::log_model_(const char *prefix) const {
  using workflow::pid::tuning_source_to_string;
  const workflow::pid::ModelState &m = this->pid_step_->modelState();
  ESP_LOGI(TAG, "%s: heating %.2f C/h (%s), cooling %.2f C/h (%s), heat loss %.3f /h (%s)", prefix,
           per_hour(m.heat_rate), tuning_source_to_string(this->sources_.heat_rate), per_hour(m.cool_rate),
           tuning_source_to_string(this->sources_.cool_rate), per_hour(m.loss),
           tuning_source_to_string(this->sources_.loss));
  ESP_LOGI(TAG, "%s: dead time heating %.1f min (%s), cooling %.1f min (%s)", prefix, m.heat_dead_s / 60.0f,
           tuning_source_to_string(this->sources_.heat_dead), m.cool_dead_s / 60.0f,
           tuning_source_to_string(this->sources_.cool_dead));
}

static bool same_model(const workflow::pid::ModelState &a, const workflow::pid::ModelState &b) {
  return a.loss == b.loss && a.heat_rate == b.heat_rate && a.cool_rate == b.cool_rate &&
         a.heat_dead_s == b.heat_dead_s && a.cool_dead_s == b.cool_dead_s;
}

void WorkflowClimate::save_tuning_(uint32_t now, bool force) {
  if (this->pid_step_ == nullptr || !this->pid_step_->isAdaptive()) {
    return;
  }
  const StoredTuning current{TUNING_VERSION, this->pid_step_->modelState(), this->seeds_};
  if (same_model(current.model, this->stored_tuning_.model) && same_model(current.seeds, this->stored_tuning_.seeds)) {
    return;
  }
  if (!force && static_cast<uint32_t>(now - this->last_tuning_save_) < TUNING_SAVE_INTERVAL_MS) {
    return;
  }
  this->stored_tuning_ = current;
  this->tuning_preference_.save(&this->stored_tuning_);
  this->last_tuning_save_ = now;
}

void WorkflowClimate::on_safe_shutdown() {
  // Runs before the preferences are synced to flash on an OTA or safe reboot.
  this->save_tuning_(millis(), true);
  this->save_soft_start_(millis(), true);
}

void WorkflowClimate::load_soft_start_() {
  if (this->pid_step_ == nullptr || !this->soft_start_configured_) {
    return;
  }
  using workflow::pid::SoftStart;
  const float seed = SoftStart::clamp_offset(this->soft_start_.initial_offset);
  this->stored_soft_start_ = StoredSoftStart{SOFT_START_VERSION, NAN, NAN, NAN};
  StoredSoftStart loaded{};
  if (this->soft_start_preference_.load(&loaded) && loaded.version == SOFT_START_VERSION) {
    this->stored_soft_start_ = loaded;
  }
  const auto heat = workflow::pid::resolve(this->stored_soft_start_.heat, this->stored_soft_start_.seed, seed,
                                           SoftStart::MIN_OFFSET, SoftStart::MAX_OFFSET);
  const auto cool = workflow::pid::resolve(this->stored_soft_start_.cool, this->stored_soft_start_.seed, seed,
                                           SoftStart::MIN_OFFSET, SoftStart::MAX_OFFSET);
  this->pid_step_->setSoftStartOffset(true, heat.value);
  this->pid_step_->setSoftStartOffset(false, cool.value);
  // Both are stored against one seed, so they come back together or not at all.
  this->soft_start_source_ = heat.source;
  ESP_LOGI(TAG, "Soft start offsets: heating %.2f C, cooling %.2f C (%s, seed %.2f C)", heat.value, cool.value,
           workflow::pid::tuning_source_to_string(heat.source), seed);
  this->save_soft_start_(millis(), true);
}

void WorkflowClimate::save_soft_start_(uint32_t now, bool force) {
  if (this->pid_step_ == nullptr || !this->soft_start_configured_) {
    return;
  }
  const StoredSoftStart current{SOFT_START_VERSION, this->pid_step_->softStartOffset(true),
                                this->pid_step_->softStartOffset(false),
                                workflow::pid::SoftStart::clamp_offset(this->soft_start_.initial_offset)};
  if (current.heat == this->stored_soft_start_.heat && current.cool == this->stored_soft_start_.cool &&
      current.seed == this->stored_soft_start_.seed) {
    return;
  }
  if (!force && static_cast<uint32_t>(now - this->last_soft_start_save_) < TUNING_SAVE_INTERVAL_MS) {
    return;
  }
  this->stored_soft_start_ = current;
  this->soft_start_preference_.save(&this->stored_soft_start_);
  this->last_soft_start_save_ = now;
}

void WorkflowClimate::set_soft_start_enabled(bool enabled) {
  if (this->pid_step_ == nullptr || !this->soft_start_configured_) {
    ESP_LOGW(TAG, "set_soft_start_enabled: soft_start is not configured");
    return;
  }
  this->soft_start_.enabled = enabled;
  this->pid_step_->setSoftStartEnabled(enabled);
  this->publish_diagnostics_();
}

void WorkflowClimate::reset_tuning() {
  if (this->pid_step_ == nullptr || !this->pid_step_->isAdaptive()) {
    ESP_LOGW(TAG, "reset_tuning: adaptive PID tuning is not enabled");
    return;
  }
  this->pid_step_->resetTuning(this->seeds_);
  this->sources_ = ModelSources{};
  this->save_tuning_(millis(), true);
  this->log_model_("Tuning reset to the configured seeds");
  this->publish_diagnostics_();
}

void WorkflowClimate::warn_if_off_step_(const char *name, float value) {
  const float step = this->traits_.get_visual_target_temperature_step();
  if (step <= 0.0f) {
    return;
  }
  const float steps = value / step;
  if (std::fabs(steps - std::round(steps)) > 0.001f) {
    ESP_LOGW(TAG,
             "%s (%.2f) is not a multiple of the device's %.2f step: the setpoint is rounded to the step and kept "
             "inside the limit, so the effective limit is %.2f",
             name, value, step, std::floor(steps + 0.001f) * step);
  }
}

void WorkflowClimate::setup() {
  if (this->inner_ == nullptr) {
    ESP_LOGE(TAG, "No inner climate configured");
    this->mark_failed();
    return;
  }

  this->device_.set_inner(this->inner_);
  this->device_.set_min_cycle_ms(this->min_cycle_ms_);
  this->device_.set_settle_ms(this->settle_ms_);
  this->device_.set_setpoint_deadband(this->pid_enabled_ ? this->setpoint_deadband_ : 0.0f);
  this->refresh_traits_();

  if (this->traits_.has_feature_flags(climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE |
                                      climate::CLIMATE_REQUIRES_TWO_POINT_TARGET_TEMPERATURE)) {
    ESP_LOGE(TAG, "Two-point target climates are not supported");
    this->mark_failed();
    return;
  }
  if (!this->traits_.supports_mode(climate::CLIMATE_MODE_HEAT) &&
      !this->traits_.supports_mode(climate::CLIMATE_MODE_COOL)) {
    ESP_LOGE(TAG, "Inner climate must support HEAT or COOL");
    this->mark_failed();
    return;
  }

  if (this->hysteresis_enabled_) {
    this->hysteresis_step_ = new (std::nothrow)
        workflow::hysterisis::HysterisisWorkflowStep(this->hysteresis_on_, this->hysteresis_off_);
    if (this->hysteresis_step_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate the hysteresis step");
      this->mark_failed();
      return;
    }
  }
  if (this->pid_enabled_) {
    this->pid_step_ = new (std::nothrow) workflow::pid::PidWorkflowStep(
        this->update_interval_, this->effective_min_temperature_(), this->effective_max_temperature_(), this->kp_,
        this->ki_, this->kd_, this->max_adjustment_under_, this->max_adjustment_over_, this->tuning_);
    if (this->pid_step_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate the PID step");
      this->mark_failed();
      return;
    }
    if (this->soft_start_configured_) {
      this->pid_step_->setSoftStart(this->soft_start_);
    }
  }

  this->targets_preference_ = this->make_entity_preference<StoredTargets>(0x57464C57UL);
  this->load_targets_();
  this->tuning_preference_ = this->make_entity_preference<StoredTuning>(TUNING_PREFERENCE_TYPE);
  this->load_tuning_();
  this->soft_start_preference_ = this->make_entity_preference<StoredSoftStart>(SOFT_START_PREFERENCE_TYPE);
  this->load_soft_start_();
  this->restored_state_ = this->restore_state_();
  this->compatible_ = true;

  if (this->inner_ready_()) {
    this->initialize_from_inner_();
  }
}

bool WorkflowClimate::inner_ready_() const {
  if (this->ready_sensor_ != nullptr && !(this->ready_sensor_->has_state() && this->ready_sensor_->state)) {
    return false;
  }
  if (this->traits_.has_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE) &&
      !std::isfinite(this->room_temperature_())) {
    return false;
  }
  return !this->mode_uses_target_(this->inner_->mode) || std::isfinite(this->inner_->target_temperature);
}

void WorkflowClimate::initialize_from_inner_() {
  const uint32_t now = millis();
  // The inner climate may only have finished describing itself during its own setup.
  this->refresh_traits_();

  const bool inner_on = this->inner_->mode != climate::CLIMATE_MODE_OFF;
  this->device_.begin(inner_on, now);

  if (!this->targets_loaded_) {
    // First boot: start from what the user last asked for, else what the device is set to.
    float seed = (this->effective_min_temperature_() + this->effective_max_temperature_()) * 0.5f;
    if (this->restored_state_.has_value() && std::isfinite(this->restored_state_->target_temperature)) {
      seed = this->restored_state_->target_temperature;
    } else if (std::isfinite(this->inner_->target_temperature)) {
      seed = this->inner_->target_temperature;
    }
    seed = this->device_.clamp_target(seed);
    this->stored_targets_.heat = seed;
    this->stored_targets_.cool = seed;
    this->targets_preference_.save(&this->stored_targets_);
    this->targets_loaded_ = true;
  }

  // A restored HEAT/COOL with the device off is the normal "workflow switched
  // it off" state: keep the user's mode and let hysteresis decide when to run.
  // In every other case the device is the source of truth.
  const bool resume_workflow = this->restored_state_.has_value() && !inner_on &&
                               this->is_workflow_mode_(this->restored_state_->mode) &&
                               this->traits_.supports_mode(this->restored_state_->mode);
  this->mode = resume_workflow ? this->restored_state_->mode : this->inner_->mode;
  if (this->is_workflow_mode_()) {
    this->target_temperature = this->stored_target_(this->mode);
  } else {
    this->target_temperature = this->inner_->target_temperature;
  }
  this->device_.set_logical(this->mode, this->target_temperature);

  this->copy_passthrough_state_();
  this->initialized_ = true;
  this->last_workflow_run_ = now;

  ESP_LOGI(TAG, "Initialized: mode=%s target=%.1f device=%s%s",
           LOG_STR_ARG(climate::climate_mode_to_string(this->mode)), this->target_temperature, ONOFF(inner_on),
           resume_workflow ? " (resuming with the device off)" : "");

  if (this->pid_enabled_) {
    this->warn_if_off_step_("pid max_adjustment_under", this->max_adjustment_under_);
    this->warn_if_off_step_("pid max_adjustment_over", this->max_adjustment_over_);
  }

  if (resume_workflow && !this->hysteresis_enabled_) {
    // Nothing else would ever switch the device back on.
    this->device_.note_power_command(true, now);
    this->device_.resend_power(now);
  }

  this->publish_state();
  this->publish_diagnostics_();
}

bool WorkflowClimate::copy_passthrough_state_() {
  bool changed = false;

  if (this->action != this->inner_->action) {
    this->action = this->inner_->action;
    changed = true;
  }
  changed |= assign_float(this->current_temperature, this->room_temperature_());
  changed |= assign_float(this->current_humidity, this->inner_->current_humidity);
  changed |= assign_float(this->target_humidity, this->inner_->target_humidity);

  if (this->inner_->has_custom_fan_mode()) {
    changed |= this->set_custom_fan_mode_(this->inner_->get_custom_fan_mode());
  } else if (this->inner_->fan_mode.has_value()) {
    changed |= this->set_fan_mode_(*this->inner_->fan_mode);
  } else if (this->fan_mode.has_value() || this->has_custom_fan_mode()) {
    this->fan_mode.reset();
    this->clear_custom_fan_mode_();
    changed = true;
  }

  if (this->swing_mode != this->inner_->swing_mode) {
    this->swing_mode = this->inner_->swing_mode;
    changed = true;
  }

  if (this->inner_->has_custom_preset()) {
    changed |= this->set_custom_preset_(this->inner_->get_custom_preset());
  } else if (this->inner_->preset.has_value()) {
    changed |= this->set_preset_(*this->inner_->preset);
  } else if (this->preset.has_value() || this->has_custom_preset()) {
    this->preset.reset();
    this->clear_custom_preset_();
    changed = true;
  }

  // Outside HEAT/COOL nothing corrects the setpoint, so the device's setpoint
  // is the user's target. While off, keep showing the last target.
  if (this->mode != climate::CLIMATE_MODE_OFF && !this->is_workflow_mode_() &&
      std::isfinite(this->inner_->target_temperature)) {
    changed |= assign_float(this->target_temperature, this->inner_->target_temperature);
  }

  return changed;
}

void WorkflowClimate::on_inner_state_() {
  if (!this->compatible_) {
    return;
  }
  if (!this->initialized_) {
    if (this->inner_ready_()) {
      this->initialize_from_inner_();
    }
    return;
  }
  // control() publishes once itself after forwarding the call.
  if (this->copy_passthrough_state_() && !this->in_control_) {
    this->publish_state();
  }
  this->publish_diagnostics_();
}

void WorkflowClimate::adopt_inner_mode_(const char *reason) {
  ESP_LOGI(TAG, "%s: following the device to %s", reason,
           LOG_STR_ARG(climate::climate_mode_to_string(this->inner_->mode)));
  this->mode = this->inner_->mode;
  if (this->is_workflow_mode_()) {
    this->target_temperature = this->stored_target_(this->mode);
  } else if (std::isfinite(this->inner_->target_temperature)) {
    this->target_temperature = this->inner_->target_temperature;
  }
  this->device_.set_logical(this->mode, this->target_temperature);
  this->copy_passthrough_state_();
  this->publish_state();
}

void WorkflowClimate::handle_power_event_(PowerEvent event, uint32_t now) {
  switch (event) {
    case PowerEvent::EXTERNAL_OFF:
      ESP_LOGI(TAG, "Device was switched off externally: turning off");
      this->mode = climate::CLIMATE_MODE_OFF;
      this->device_.set_logical(this->mode, this->target_temperature);
      this->copy_passthrough_state_();
      this->publish_state();
      break;
    case PowerEvent::EXTERNAL_ON:
      this->adopt_inner_mode_("Device was switched on externally");
      break;
    case PowerEvent::UNEXPECTED_ON:
      ESP_LOGE(TAG, "Device is on although the workflow switched it off; internal power state resynced");
      break;
    case PowerEvent::LOST_COMMAND:
      if (static_cast<uint32_t>(now - this->device_.last_power_command_ms()) >= this->min_cycle_ms_) {
        ESP_LOGW(TAG, "Device did not follow the last power command (%s); sending it again",
                 ONOFF(this->device_.isInternalPowerOn()));
        this->device_.resend_power(now);
      }
      break;
    case PowerEvent::CONFIRMED:
    case PowerEvent::NONE:
      return;
  }
  this->publish_diagnostics_();
}

void WorkflowClimate::run_workflows_() {
  if (!this->is_workflow_mode_()) {
    return;
  }
  const float current = this->room_temperature_();
  if (!std::isfinite(current) || !std::isfinite(this->target_temperature)) {
    return;
  }
  if (this->hysteresis_step_ != nullptr) {
    this->hysteresis_step_->run(current, &this->device_);
  }
  if (this->pid_step_ != nullptr) {
    this->pid_step_->setOutdoorTemperature(this->outdoor_temperature_sensor_ != nullptr &&
                                                   this->outdoor_temperature_sensor_->has_state()
                                               ? this->outdoor_temperature_sensor_->state
                                               : NAN);
    this->pid_step_->run(current, &this->device_);
    this->save_tuning_(millis(), false);
    this->save_soft_start_(millis(), false);
  } else {
    this->device_.sync_target();
  }
  this->publish_diagnostics_();
}

void WorkflowClimate::loop() {
  if (!this->compatible_) {
    return;
  }
  if (!this->initialized_) {
    if (this->inner_ready_()) {
      this->initialize_from_inner_();
    }
    return;
  }
  if (!this->inner_ready_()) {
    return;
  }

  const uint32_t now = millis();
  const bool outer_on = this->mode != climate::CLIMATE_MODE_OFF;

  // Power first, so an external OFF is seen before hysteresis can undo it.
  this->handle_power_event_(this->device_.observe(outer_on, now), now);

  // The device changed between two running modes (e.g. HEAT -> COOL on the remote).
  if (this->mode != climate::CLIMATE_MODE_OFF && this->device_.isInternalPowerOn() &&
      !this->device_.settling(now) && this->device_.device_power_on() && this->inner_->mode != this->mode) {
    this->adopt_inner_mode_("Device mode was changed externally");
  }

  if (static_cast<uint32_t>(now - this->last_workflow_run_) >= this->update_interval_) {
    this->last_workflow_run_ = now;
    this->run_workflows_();
  }
}

void WorkflowClimate::control(const climate::ClimateCall &call) {
  if (!this->initialized_) {
    ESP_LOGW(TAG, "Ignoring control: the inner climate has not reported its state yet");
    return;
  }
  const uint32_t now = millis();

  const bool has_mode = call.get_mode().has_value();
  const bool mode_changed = has_mode && *call.get_mode() != this->mode;
  const bool has_target = call.get_target_temperature().has_value();
  if (has_mode) {
    this->mode = *call.get_mode();
  }
  const bool workflow_mode = this->is_workflow_mode_();

  if (has_target) {
    this->target_temperature = this->device_.clamp_target(*call.get_target_temperature());
  } else if (mode_changed && workflow_mode) {
    this->target_temperature = this->stored_target_(this->mode);
  }
  if (workflow_mode && std::isfinite(this->target_temperature)) {
    this->store_target_(this->mode, this->target_temperature);
  }
  this->device_.set_logical(this->mode, this->target_temperature);

  auto inner_call = this->inner_->make_call();
  bool forward = false;

  // Re-selecting the current HEAT/COOL mode must not restart a device the
  // workflow has switched off.
  const bool send_mode = has_mode && (mode_changed || !workflow_mode);
  if (send_mode) {
    inner_call.set_mode(this->mode);
    forward = true;
  }
  if (std::isfinite(this->target_temperature) && this->mode_uses_target_(this->mode) &&
      ((send_mode && workflow_mode) || (has_target && !workflow_mode))) {
    const float quantized = this->device_.quantize_target(this->target_temperature);
    inner_call.set_target_temperature(quantized);
    this->device_.note_target_sent(quantized, now);
    forward = true;
  }

  if (call.has_custom_fan_mode()) {
    this->set_custom_fan_mode_(call.get_custom_fan_mode());
    inner_call.set_fan_mode(call.get_custom_fan_mode());
    forward = true;
  } else if (call.get_fan_mode().has_value()) {
    this->set_fan_mode_(*call.get_fan_mode());
    inner_call.set_fan_mode(*call.get_fan_mode());
    forward = true;
  }
  if (call.get_swing_mode().has_value()) {
    this->swing_mode = *call.get_swing_mode();
    inner_call.set_swing_mode(*call.get_swing_mode());
    forward = true;
  }
  if (call.has_custom_preset()) {
    this->set_custom_preset_(call.get_custom_preset());
    inner_call.set_preset(call.get_custom_preset());
    forward = true;
  } else if (call.get_preset().has_value()) {
    this->set_preset_(*call.get_preset());
    inner_call.set_preset(*call.get_preset());
    forward = true;
  }
  if (call.get_target_humidity().has_value()) {
    this->target_humidity = *call.get_target_humidity();
    inner_call.set_target_humidity(*call.get_target_humidity());
    forward = true;
  }

  if (send_mode) {
    this->device_.note_power_command(this->mode != climate::CLIMATE_MODE_OFF, now);
  }
  if (forward) {
    this->in_control_ = true;
    inner_call.perform();
    this->in_control_ = false;
  }

  // Let the workflows pick up a new target on the next loop instead of waiting an interval.
  if (workflow_mode) {
    this->last_workflow_run_ = now - this->update_interval_;
  }

  this->publish_state();
  this->publish_diagnostics_();
}

void WorkflowClimate::publish_diagnostics_() {
  if (!this->initialized_) {
    return;
  }
  publish_binary(this->internal_power_sensor_, this->device_.isInternalPowerOn());
  publish_binary(this->device_power_sensor_, this->device_.device_power_on());
  publish_binary(this->operating_sensor_, this->device_.device_operating());
  publish_value(this->device_current_temperature_sensor_, this->inner_->current_temperature);
  publish_value(this->device_set_point_sensor_, this->inner_->target_temperature);
  publish_value(this->pid_set_point_sensor_, this->device_.corrected_target());
  if (this->pid_step_ != nullptr) {
    publish_value(this->pid_kp_sensor_, this->pid_step_->kp());
    publish_value(this->pid_ki_sensor_, this->pid_step_->ki());
    const workflow::pid::ModelState &m = this->pid_step_->modelState();
    publish_value(this->heating_rate_sensor_, per_hour(m.heat_rate));
    publish_value(this->cooling_rate_sensor_, per_hour(m.cool_rate));
    publish_value(this->heat_loss_sensor_, per_hour(m.loss));
    publish_value(this->dead_time_heating_sensor_, m.heat_dead_s / 60.0f);
    publish_value(this->dead_time_cooling_sensor_, m.cool_dead_s / 60.0f);
    if (this->soft_start_configured_) {
      publish_value(this->soft_start_offset_heating_sensor_, this->pid_step_->softStartOffset(true));
      publish_value(this->soft_start_offset_cooling_sensor_, this->pid_step_->softStartOffset(false));
      publish_value(this->soft_start_overshoot_sensor_, this->pid_step_->softStartLastOvershoot());
      publish_binary(this->soft_start_active_sensor_, this->pid_step_->softStartActive());
    }
  }
}

void WorkflowClimate::dump_config() {
  LOG_CLIMATE("", "Workflow Climate", this);
  ESP_LOGCONFIG(TAG, "  Update interval: %u ms", static_cast<unsigned>(this->update_interval_));
  ESP_LOGCONFIG(TAG, "  Settle time: %u ms", static_cast<unsigned>(this->settle_ms_));
  ESP_LOGCONFIG(TAG, "  Room temperature: %s", this->temperature_sensor_ != nullptr ? "sensor" : "inner climate");
  if (this->pid_enabled_) {
    ESP_LOGCONFIG(TAG, "  PID: kp=%.3f ki=%.4f kd=%.3f, adjustment -%.2f/+%.2f, setpoint deadband %.2f", this->kp_,
                  this->ki_, this->kd_, this->max_adjustment_under_, this->max_adjustment_over_,
                  this->setpoint_deadband_);
    if (this->tuning_.adaptive) {
      ESP_LOGCONFIG(TAG, "  PID tuning: adaptive, response time %.0f s, dead time %.0f s, kp %.2f..%.2f",
                    this->tuning_.response_time_s, this->tuning_.dead_time_s, this->tuning_.kp_min,
                    this->tuning_.kp_max);
      if (this->pid_step_ != nullptr) {
        ESP_LOGCONFIG(TAG, "  PID gains in use: kp: %.3f  ki: %.5f  kd: %.3f", this->pid_step_->kp(),
                      this->pid_step_->ki(), this->pid_step_->kd());
        using workflow::pid::tuning_source_to_string;
        const workflow::pid::ModelState &m = this->pid_step_->modelState();
        ESP_LOGCONFIG(TAG, "  Heating rate: %.2f C/h, started %s (seed %.2f C/h)", per_hour(m.heat_rate),
                      tuning_source_to_string(this->sources_.heat_rate), per_hour(this->seeds_.heat_rate));
        ESP_LOGCONFIG(TAG, "  Cooling rate: %.2f C/h, started %s (seed %.2f C/h)", per_hour(m.cool_rate),
                      tuning_source_to_string(this->sources_.cool_rate), per_hour(this->seeds_.cool_rate));
        ESP_LOGCONFIG(TAG, "  Heat loss: %.3f /h, started %s (seed %.3f /h), outdoor temperature: %s",
                      per_hour(m.loss), tuning_source_to_string(this->sources_.loss), per_hour(this->seeds_.loss),
                      this->outdoor_temperature_sensor_ != nullptr ? "sensor" : "none (loss not learned)");
        ESP_LOGCONFIG(TAG, "  Dead time: heating %.1f min (%s), cooling %.1f min (%s), seed %.1f min",
                      m.heat_dead_s / 60.0f, tuning_source_to_string(this->sources_.heat_dead), m.cool_dead_s / 60.0f,
                      tuning_source_to_string(this->sources_.cool_dead), this->seeds_.heat_dead_s / 60.0f);
      }
    } else {
      ESP_LOGCONFIG(TAG, "  PID tuning: fixed gains");
    }
    if (this->soft_start_configured_ && this->pid_step_ != nullptr) {
      ESP_LOGCONFIG(TAG,
                    "  Soft start: %s, offset below the target heating %.2f C, cooling %.2f C (%s, seed %.2f C), "
                    "hold %u s, ramp %.2f C/min",
                    this->pid_step_->softStartEnabled() ? "enabled" : "disabled", this->pid_step_->softStartOffset(true),
                    this->pid_step_->softStartOffset(false),
                    workflow::pid::tuning_source_to_string(this->soft_start_source_),
                    this->soft_start_.initial_offset, static_cast<unsigned>(this->soft_start_.hold_ms / 1000),
                    this->soft_start_.rate_per_s * 60.0f);
    } else {
      ESP_LOGCONFIG(TAG, "  Soft start: not configured");
    }
    this->warn_if_off_step_("pid max_adjustment_under", this->max_adjustment_under_);
    this->warn_if_off_step_("pid max_adjustment_over", this->max_adjustment_over_);
  } else {
    ESP_LOGCONFIG(TAG, "  PID: disabled");
  }
  if (this->hysteresis_enabled_) {
    ESP_LOGCONFIG(TAG, "  Hysteresis: on=%.2f off=%.2f, minimum cycle %u ms", this->hysteresis_on_,
                  this->hysteresis_off_, static_cast<unsigned>(this->min_cycle_ms_));
  } else {
    ESP_LOGCONFIG(TAG, "  Hysteresis: disabled");
  }
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup failed: the inner climate is missing or not a single-target HEAT/COOL climate");
  }
}

}  // namespace workflow_climate
}  // namespace esphome
