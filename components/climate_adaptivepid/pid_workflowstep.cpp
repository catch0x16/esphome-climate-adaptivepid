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

        void PidWorkflowStep::run(const float currentTemperature, devicestate::IDeviceStateManager* deviceManager) {
            if (deviceManager == nullptr) {
                ESP_LOGW(TAG, "run: deviceManager is null");
                return;
            }
            const bool updatedPidTarget = this->ensurePIDTarget(deviceManager);

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

            if (this->startControl) {
                this->runStartControl(currentTemperature, deviceManager);
                return;
            }

            // if pid target is not updated and internal power is not on
            if (updatedPidTarget || deviceManager->isInternalPowerOn()) {
                const float adjustMinOffset = deviceManager->getOffsetDirection()
                    ? this->maxAdjustmentUnder
                    : this->maxAdjustmentOver;
                const float adjustMaxOffset = deviceManager->getOffsetDirection()
                    ? this->maxAdjustmentOver
                    : this->maxAdjustmentUnder;
                const float target = deviceManager->getTargetTemperature();
                const float adjustedMinTemp = devicestate::clamp(target - adjustMinOffset, this->minTemp, this->maxTemp);
                const float adjustedMaxTemp = devicestate::clamp(target + adjustMaxOffset, this->minTemp, this->maxTemp);

                ESP_LOGI(TAG, "PidWorkflowStep (Device) target={%.2f} adjustedMin={%.2f} adjustedMax={%.2f} powerOn={%s}",
                    this->adaptivePID->get_target(), adjustedMinTemp, adjustedMaxTemp, YESNO(deviceManager->isInternalPowerOn()));

                unsigned long now = esphome::millis();

                // The PID knows the range it may use, so its integral stops
                // accumulating once the setpoint is pinned at either end.
                this->adaptivePID->set_output_limits(adjustedMinTemp - target, adjustedMaxTemp - target);
                const bool powerOn = deviceManager->isInternalPowerOn();
                const bool holding = this->holdLeast && powerOn;
                const float setPointCorrectionSimple = holding
                    ? this->adaptivePID->hold(currentTemperature, now)
                    : this->adaptivePID->update(currentTemperature, now, powerOn);
                const float adjustedSetPointCorrectionSimple = devicestate::clamp(setPointCorrectionSimple, adjustedMinTemp, adjustedMaxTemp);
                ESP_LOGI(TAG, "PidWorkflowStep [Simple] setPointCorrection={%.2f} adjustedSetPoint={%.2f}%s kp={%.3f} ki={%.4f} kd={%.3f}",
                    setPointCorrectionSimple, adjustedSetPointCorrectionSimple, holding ? " (held at least output)" : "",
                    this->adaptivePID->kp(), this->adaptivePID->ki(), this->adaptivePID->kd());

                if (deviceManager->internalSetCorrectedTemperature(
                        adjustedSetPointCorrectionSimple, adjustedMinTemp, adjustedMaxTemp)) {
                    deviceManager->commit();
                }
            }
        }

        void PidWorkflowStep::configureStartControl(const workflow::start::StartConfig &config) {
            this->startControl = true;
            this->startConfig = config;
            this->startCtl.configure(config);
        }

        void PidWorkflowStep::setSharedHeatHint(bool active, uint32_t validMs) {
            this->detector.set_hint(active, esphome::millis(), validMs);
            ESP_LOGI(TAG, "Supervisor: shared heat %s for %u s", active ? "on" : "off",
                static_cast<unsigned>(validMs / 1000));
        }

        bool PidWorkflowStep::sharedHeat() const { return this->detector.active(esphome::millis()); }

        void PidWorkflowStep::setSharedHeatExpected(int32_t expectedInS, uint32_t validMs) {
            this->detector.set_expected_hint(expectedInS, esphome::millis(), validMs);
            if (expectedInS >= 0) {
                ESP_LOGI(TAG, "Supervisor: shared heat expected in %d s (for %u s)", static_cast<int>(expectedInS),
                    static_cast<unsigned>(validMs / 1000));
            } else {
                ESP_LOGI(TAG, "Supervisor: no shared heat expected");
            }
        }

        float PidWorkflowStep::sharedHeatExpectedIn() const { return this->detector.expected_in_s(esphome::millis()); }

        void PidWorkflowStep::runStartControl(const float currentTemperature, devicestate::IDeviceStateManager* deviceManager) {
            using namespace workflow::start;
            const uint32_t now = esphome::millis();
            const bool heating = deviceManager->getOffsetDirection();
            const float target = deviceManager->getTargetTemperature();
            const float adjustMinOffset = heating ? this->maxAdjustmentUnder : this->maxAdjustmentOver;
            const float adjustMaxOffset = heating ? this->maxAdjustmentOver : this->maxAdjustmentUnder;
            const float adjustedMinTemp = devicestate::clamp(target - adjustMinOffset, this->minTemp, this->maxTemp);
            const float adjustedMaxTemp = devicestate::clamp(target + adjustMaxOffset, this->minTemp, this->maxTemp);
            this->adaptivePID->set_output_limits(adjustedMinTemp - target, adjustedMaxTemp - target);

            const bool powered = deviceManager->isInternalPowerOn();
            const devicestate::DeviceStatus status = deviceManager->getDeviceStatus();
            const bool operating = powered && status.operating;

            // The PID's output is the setpoint it asks for; start control compares
            // it with the room and sends the matching level (start_controller.h).
            // The PID keeps running while the head is parked, so its output is
            // current once the head is switched back on; its integral is frozen
            // only while parked.
            const float output = this->adaptivePID->update(currentTemperature, now, true);
            this->startOutput = output;

            // Shared heat: only while the head is powered, idle and past its own coast.
            const bool eligible = powered && !operating && this->startCtl.state() == HeadState::IDLE &&
                                  !this->bursts.coasting() &&
                                  this->startCtl.since_stop_ms(now) >= this->bursts.coast_time_ms();
            this->detector.update(now, heating, eligible, currentTemperature, this->humidity);
            const bool shared = this->detector.active(now);

            StartInput in;
            in.now_ms = now;
            in.heating = heating;
            in.powered = powered;
            in.operating = operating;
            in.device_view = status.currentTemperature;
            in.room = currentTemperature;
            in.target = target;
            in.output = output;
            in.shared_heat = shared;
            in.shared_expected_s = this->detector.expected_in_s(now);
            in.hold = this->holdLeast && powered;
            const StartDecision decision = this->startCtl.update(in);
            this->adaptivePID->set_integral_frozen(decision.freeze_integral);

            if (this->bursts.update(now, heating, decision.state, powered, currentTemperature, shared)) {
                const BurstState &b = this->bursts.state();
                ESP_LOGI(TAG, "Start control learned: gain alone %.2f (%u runs), shared %.2f (%u runs); coast alone %.2f, shared %.2f, after %.1f min",
                    b.gain[0], static_cast<unsigned>(b.gain_count[0]), b.gain[1], static_cast<unsigned>(b.gain_count[1]),
                    b.coast[0], b.coast[1], b.coast_time_s / 60.0f);
            }
            if (decision.transition == Transition::START_FAULT) {
                ESP_LOGW(TAG, "Start control: %s (room %.2f, PID output %.2f, setpoint %.1f, unit reports %.1f)",
                    transition_to_string(decision.transition), currentTemperature, output, decision.setpoint,
                    status.currentTemperature);
            } else if (decision.transition != Transition::NONE) {
                ESP_LOGI(TAG, "Start control: %s -> %s (room %.2f, target %.2f, PID output %.2f, %s level, setpoint %.1f, unit reports %.1f, shared heat %s%s)",
                    transition_to_string(decision.transition), head_state_to_string(decision.state),
                    currentTemperature, target, output, level_to_string(decision.level), decision.setpoint,
                    status.currentTemperature, YESNO(shared), this->detector.hint_valid(now) ? " (supervisor)" : "");
            }

            switch (decision.power) {
                case PowerRequest::TURN_OFF:
                    deviceManager->internalTurnOff();
                    break;
                case PowerRequest::TURN_ON:
                    deviceManager->internalTurnOnAt(decision.setpoint);
                    break;
                case PowerRequest::NONE:
                    if (powered && std::isfinite(decision.setpoint) &&
                        deviceManager->internalSetDeviceTemperature(decision.setpoint)) {
                        deviceManager->commit();
                    }
                    break;
            }
            ESP_LOGD(TAG, "Start control %s: room %.2f, PID output %.2f (%s level), setpoint %.1f, shared heat %s, expected in %.0f s",
                head_state_to_string(decision.state), currentTemperature, output, level_to_string(decision.level),
                decision.setpoint, YESNO(shared), in.shared_expected_s);
        }

    }
}