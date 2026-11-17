#include "hysterisis_workflowstep.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
using namespace esphome;

using namespace devicestate;

namespace workflow {

    namespace hysterisis {

        static const char* TAG = "HysterisisWorkflowStep"; // Logging tag

        HysterisisWorkflowStep::HysterisisWorkflowStep(
            const float hysterisisOn,
            const float hysterisisOff,
            const float offLimit,
            const uint32_t offDelayMs
        ) : offGate(hysterisisOff, offLimit, offDelayMs) {
            this->hysterisisOn = hysterisisOn;
            this->hysterisisOff = hysterisisOff;
        }
        
        void HysterisisWorkflowStep::executeHysterisisWorkflowStep(
                HysterisisResult* result, devicestate::IDeviceStateManager* deviceManager) {
            if (result == nullptr || deviceManager == nullptr) {
                ESP_LOGW(TAG, "executeHysterisisWorkflowStep: null parameter");
                return;
            }
            if (result->active) {
                ESP_LOGV(TAG, "Active while %s: delta={%f} current={%f} targetTemperature={%f}", result->label.c_str(), result->delta, result->currentTemperature, result->targetTemperature);
                const uint32_t now = millis();
                const bool wasHolding = this->holding;
                const OffAction action = this->offGate.update(result->delta, now);
                this->holding = action != OffAction::NONE;
                if (action == OffAction::HOLD && !wasHolding) {
                    ESP_LOGI(TAG, "Holding at least output while %s: delta={%f} current={%f} targetTemperature={%f}", result->label.c_str(), result->delta, result->currentTemperature, result->targetTemperature);
                } else if (action == OffAction::NONE && wasHolding) {
                    ESP_LOGI(TAG, "Hold released while %s: delta={%f}", result->label.c_str(), result->delta);
                } else if (action == OffAction::TURN_OFF) {
                    ESP_LOGI(TAG, "Turn off while %s after %.0f s past the threshold: delta={%f} current={%f} targetTemperature={%f}", result->label.c_str(), this->offGate.held_ms(now) / 1000.0f, result->delta, result->currentTemperature, result->targetTemperature);
                    if (deviceManager->internalTurnOff()) {
                        this->offGate.reset();
                        this->holding = false;
                    }
                }
            } else {
                this->offGate.reset();
                this->holding = false;
                ESP_LOGI(TAG, "Not currently active while %s: delta={%f} current={%f} targetTemperature={%f}", result->label.c_str(), result->delta, result->currentTemperature, result->targetTemperature);
                if (-result->delta > this->hysterisisOn) {
                    ESP_LOGI(TAG, "Turn on while %s: delta={%f} current={%f} targetTemperature={%f}", result->label.c_str(), result->delta, result->currentTemperature, result->targetTemperature);
                    deviceManager->internalTurnOn();
                }
            }
        }
        
        HysterisisResult HysterisisWorkflowStep::getHysterisisResult(
                const float currentTemperature, devicestate::IDeviceStateManager* deviceManager) {
            HysterisisResult result;
            result.shouldRun = false;  // Default to not running

            if (deviceManager == nullptr) {
                ESP_LOGW(TAG, "getHysterisisResult: deviceManager is null");
                return result;
            }

            const float targetTemperature = deviceManager->getTargetTemperature();
        
            const DeviceState deviceState = deviceManager->getDeviceState();
            result.currentTemperature = currentTemperature;
            result.targetTemperature = targetTemperature;
            result.active = deviceState.active;
        
            switch(deviceState.mode) {
                case devicestate::DeviceMode::DeviceMode_Heat: {
                    const float delta = currentTemperature - targetTemperature;
                    result.shouldRun = true;
                    result.delta = delta;
                    result.label = "heating";
                    break;
                }
                case devicestate::DeviceMode::DeviceMode_Cool: {
                    const float delta = targetTemperature - currentTemperature;
                    result.shouldRun = true;
                    result.delta = delta;
                    result.label = "cooling";
                    break;
                }
                default: {
                    ESP_LOGI(workflow::hysterisis::TAG, "Doing nothing in current mode (%s): current={%f} targetTemperature={%f}", 
                        devicestate::deviceModeToString(deviceState.mode), currentTemperature, targetTemperature);
                    result.shouldRun = false;
                }
            }
        
            return result;
        }
        
        void HysterisisWorkflowStep::run(const float currentTemperature, devicestate::IDeviceStateManager* deviceManager) {
            if (deviceManager == nullptr) {
                ESP_LOGW(TAG, "run: deviceManager is null");
                return;
            }
            HysterisisResult result = this->getHysterisisResult(currentTemperature, deviceManager);
            if (result.shouldRun) {
                this->executeHysterisisWorkflowStep(&result, deviceManager);
            } else {
                this->offGate.reset();
                this->holding = false;
            }
        }

    }

}
