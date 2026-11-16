#include "pid_workflowstep.h"

#include <new>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
using namespace esphome;

#include "floats.h"
using namespace devicestate;

namespace workflow {

    namespace pid {

        static const char* TAG = "PidWorkflowStep"; // Logging tag

        PidWorkflowStep::PidWorkflowStep(
            const uint32_t updateInterval,
            const float minTemp,
            const float maxTemp,
            const float p,
            const float i,
            const float d,
            const float maxAdjustmentUnder,
            const float maxAdjustmentOver,
            const TuningConfig &tuning
        ) {
            this->minTemp = minTemp;
            this->maxTemp = maxTemp;
            this->maxAdjustmentUnder = maxAdjustmentUnder;
            this->maxAdjustmentOver = maxAdjustmentOver;

            this->tuning = tuning;
            this->configuredKi = i;
            this->useConfiguredKi[0] = i > 0.0f;
            this->useConfiguredKi[1] = i > 0.0f;
            this->adaptivePID = new AdaptivePID(p, i, d);

            if (this->tuning.adaptive) {
                // A configured kp is the starting point: it implies a heating
                // and cooling rate, which measurements then refine. Without one
                // the model's default rate is used.
                ModelState state = this->model.state();
                if (p > 0.0f) {
                    state.heat_rate = rate_from_kp(p, this->tuning);
                    state.cool_rate = state.heat_rate;
                }
                state.heat_dead_s = this->tuning.dead_time_s;
                state.cool_dead_s = this->tuning.dead_time_s;
                this->model.set_state(state);
                this->applyTunedGains(true);
            }
        }

        void PidWorkflowStep::applyTunedGains(bool heating) {
            const Gains gains = gains_from_rate(this->model.rate(heating), this->model.dead_time_s(heating), this->tuning);
            const bool keepConfiguredKi = this->useConfiguredKi[heating ? 1 : 0];
            this->adaptivePID->set_gains(gains.kp, keepConfiguredKi ? this->configuredKi : gains.ki);
        }

        void PidWorkflowStep::startModel(const ModelState &state, bool restoredHeat, bool restoredCool) {
            this->model.set_state(state);
            this->useConfiguredKi[1] = !restoredHeat && this->configuredKi > 0.0f;
            this->useConfiguredKi[0] = !restoredCool && this->configuredKi > 0.0f;
            this->applyTunedGains(this->heating);
        }

        void PidWorkflowStep::resetTuning(const ModelState &seeds) {
            this->model = RoomModel{};
            this->startModel(seeds, false, false);
        }

        bool PidWorkflowStep::ensurePIDTarget(devicestate::IDeviceStateManager* deviceManager) {
            if (deviceManager == nullptr || this->adaptivePID == nullptr) {
                ESP_LOGW(TAG, "ensurePIDTarget: null parameter");
                return false;
            }
            if (devicestate::same_float(deviceManager->getTargetTemperature(), this->adaptivePID->get_target(), 0.01f) &&
                deviceManager->getOffsetDirection() == this->adaptivePID->heating()) {
                return false;
            }

            ESP_LOGW(TAG, "Simple PID target temp changing from %f to %f", this->adaptivePID->get_target(), deviceManager->getTargetTemperature());
            this->adaptivePID->set_target(deviceManager->getTargetTemperature(), deviceManager->getOffsetDirection());
            return true;
        }

        void PidWorkflowStep::setSoftStartEnabled(bool enabled) {
            if (enabled != this->softStart.enabled()) {
                ESP_LOGI(TAG, "Soft start %s", enabled ? "enabled" : "disabled");
            }
            this->softStart.set_enabled(enabled);
        }

        float PidWorkflowStep::updateSoftStart(const float currentTemperature, const bool demandRaised, const float maxOutput,
                                               devicestate::IDeviceStateManager* deviceManager, uint32_t now) {
            const bool powerOn = deviceManager->isInternalPowerOn();
            const bool operating = powerOn && deviceManager->getDeviceStatus().operating;
            const bool heatingNow = deviceManager->getOffsetDirection();
            const float sign = heatingNow ? 1.0f : -1.0f;
            const float target = deviceManager->getTargetTemperature();
            const float deviceSetpoint = deviceManager->getDeviceState().targetTemperature;
            // How far the room is short of the target, and how far the device's
            // setpoint is past the room, both in the direction of more output.
            const float error = sign * (target - currentTemperature);
            const float call = sign * (deviceSetpoint - currentTemperature);
            const bool demand = std::isfinite(error) && error > 0.0f;

            // The step does not run outside HEAT/COOL. After a gap (first run,
            // or the outer climate was off) the device counts as having been
            // off, so a running one is soft started from where it runs, and a
            // start that happened during the gap is not learned from.
            if (!this->hasPrevious || now - this->previousRunMs > MAX_SOFT_START_GAP_MS) {
                this->previousPowerOn = false;
                this->previousOperating = operating;
                this->previousDemand = false;
            }

            // The device's own start: learn where it started, while a soft start runs.
            if (operating && !this->previousOperating && this->softStart.on_device_start(heatingNow, call)) {
                ESP_LOGI(TAG, "Soft start: device started at a call of %.2f C, start point now %.2f C (%s)",
                    call, this->softStart.start_point(heatingNow), heatingNow ? "heating" : "cooling");
            }
            if (!powerOn) {
                this->softStart.cancel();
            } else {
                const char *reason = nullptr;
                if (!this->previousPowerOn) {
                    reason = "switched on";
                } else if (demandRaised) {
                    reason = "target raised";
                } else if (!operating && demand && !this->previousDemand) {
                    reason = "demand began";
                }
                if (reason != nullptr && this->softStart.enabled()) {
                    // A running device is kept where it is and raised from there.
                    const float current = sign * (deviceSetpoint - target);
                    this->softStart.arm(now, heatingNow, error, operating, current);
                    ESP_LOGI(TAG, "Soft start (%s): output capped at %.2f C from the target, start point %.2f C",
                        reason, this->softStart.cap(now, maxOutput), this->softStart.start_point(heatingNow));
                }
            }
            this->hasPrevious = true;
            this->previousRunMs = now;
            this->previousPowerOn = powerOn;
            this->previousOperating = operating;
            this->previousDemand = demand;
            return this->softStart.cap(now, maxOutput);
        }

        void PidWorkflowStep::run(const float currentTemperature, devicestate::IDeviceStateManager* deviceManager) {
            if (deviceManager == nullptr) {
                ESP_LOGW(TAG, "run: deviceManager is null");
                return;
            }
            // A target change towards more output (or a change of direction) is a new demand.
            const float previousTarget = this->adaptivePID->get_target();
            const bool previousHeating = this->adaptivePID->heating();
            const bool updatedPidTarget = this->ensurePIDTarget(deviceManager);
            const bool demandRaised = updatedPidTarget &&
                (this->adaptivePID->heating() != previousHeating ||
                 (this->adaptivePID->heating() ? this->adaptivePID->get_target() > previousTarget
                                               : this->adaptivePID->get_target() < previousTarget));

            if (this->tuning.adaptive) {
                // Learn how the room responds, and retune from it.
                const bool nowHeating = deviceManager->getOffsetDirection();
                const devicestate::DeviceStatus status = deviceManager->getDeviceStatus();
                const bool operating = deviceManager->isInternalPowerOn() && status.operating;
                const float output = estimate_output(operating, nowHeating, deviceManager->getDeviceState().targetTemperature,
                                                     currentTemperature, this->tuning.full_output_gap);
                const ModelUpdate update = this->model.update(ModelSample{esphome::millis(), currentTemperature,
                    this->outdoorTemperature, nowHeating, operating, output, operating && status.defrosting});
                if (update.rate) {
                    this->useConfiguredKi[nowHeating ? 1 : 0] = false;
                }
                if (update.loss) {
                    ESP_LOGI(TAG, "Measured heat loss: %.3f /h", this->model.loss() * 3600.0f);
                }
                if (update.anomaly) {
                    ESP_LOGI(TAG, "Dropped a %s window: the room moved the wrong way faster than with the device off",
                        nowHeating ? "heating" : "cooling");
                }
                if (update.rate || update.dead_time || nowHeating != this->heating) {
                    this->heating = nowHeating;
                    this->applyTunedGains(nowHeating);
                    if (update.rate || update.dead_time) {
                        ESP_LOGI(TAG, "Tuned from %s rate %.2f C/h, dead time %.1f min: kp: %.3f  ki: %.5f  kd: %.3f",
                            nowHeating ? "heating" : "cooling", this->model.rate(nowHeating) * 3600.0f,
                            this->model.dead_time_s(nowHeating) / 60.0f,
                            this->adaptivePID->kp(), this->adaptivePID->ki(), this->adaptivePID->kd());
                    }
                }
            }

            const float adjustMinOffset = deviceManager->getOffsetDirection()
                ? this->maxAdjustmentUnder
                : this->maxAdjustmentOver;
            const float adjustMaxOffset = deviceManager->getOffsetDirection()
                ? this->maxAdjustmentOver
                : this->maxAdjustmentUnder;
            const float target = deviceManager->getTargetTemperature();
            const float adjustedMinTemp = devicestate::clamp(target - adjustMinOffset, this->minTemp, this->maxTemp);
            const float adjustedMaxTemp = devicestate::clamp(target + adjustMaxOffset, this->minTemp, this->maxTemp);
            const uint32_t now = esphome::millis();

            // Every run, so power and demand changes are seen while the device is off too.
            const float maxOutput = deviceManager->getOffsetDirection() ? adjustedMaxTemp - target : target - adjustedMinTemp;
            const float cap = this->updateSoftStart(currentTemperature, demandRaised, maxOutput, deviceManager, now);

            // if pid target is not updated and internal power is not on
            if (updatedPidTarget || deviceManager->isInternalPowerOn()) {
                ESP_LOGI(TAG, "PidWorkflowStep (Device) target={%.2f} adjustedMin={%.2f} adjustedMax={%.2f} powerOn={%s}",
                    this->adaptivePID->get_target(), adjustedMinTemp, adjustedMaxTemp, YESNO(deviceManager->isInternalPowerOn()));

                // The PID knows the range it may use, so its integral stops
                // accumulating once the setpoint is pinned at either end.
                this->adaptivePID->set_output_limits(adjustedMinTemp - target, adjustedMaxTemp - target);
                this->adaptivePID->set_output_cap(cap);
                const float setPointCorrectionSimple =
                    this->adaptivePID->update(currentTemperature, now, deviceManager->isInternalPowerOn());
                const float adjustedSetPointCorrectionSimple = devicestate::clamp(setPointCorrectionSimple, adjustedMinTemp, adjustedMaxTemp);
                ESP_LOGI(TAG, "PidWorkflowStep [Simple] setPointCorrection={%.2f} adjustedSetPoint={%.2f} kp={%.3f} ki={%.4f} kd={%.3f}",
                    setPointCorrectionSimple, adjustedSetPointCorrectionSimple,
                    this->adaptivePID->kp(), this->adaptivePID->ki(), this->adaptivePID->kd());

                if (deviceManager->internalSetCorrectedTemperature(
                        adjustedSetPointCorrectionSimple, adjustedMinTemp, adjustedMaxTemp)) {
                    deviceManager->commit();
                }
            }
        }

    }
}