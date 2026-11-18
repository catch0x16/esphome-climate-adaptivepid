#include <string>

#include <cstdint>

#include "controller_types.h"
#include "off_gate.h"

#ifndef HYSTERISIS_WORKFLOWSTEP_H
#define HYSTERISIS_WORKFLOWSTEP_H

namespace workflow {

    namespace hysterisis {

        struct HysterisisResult {
            float currentTemperature;
            float targetTemperature;
            float delta;
            bool active;
            std::string label;
            bool shouldRun;
        };
        
        class HysterisisWorkflowStep : public devicestate::WorkflowStep {
        private:
            float hysterisisOn;
            float hysterisisOff;
            OffGate offGate;
            bool holding{false};
            bool turnOnEnabled{true};
        
            void executeHysterisisWorkflowStep(HysterisisResult* result, devicestate::IDeviceStateManager* deviceManager);
            HysterisisResult getHysterisisResult(const float currentTemperature, devicestate::IDeviceStateManager* deviceManager);
        
        public:
            // offLimit: switch off at once past this (NaN: no limit). offDelayMs:
            // how long the room must stay past hysterisisOff before switching off;
            // meanwhile the step is holding (see OffGate).
            HysterisisWorkflowStep(
                const float hysterisisOn,
                const float hysterisisOff,
                const float offLimit,
                const uint32_t offDelayMs
            );
        
            void run(const float currentTemperature, devicestate::IDeviceStateManager* deviceManager) override;

            // Past the off threshold and waiting out the delay: the PID should
            // hold the device at its least output.
            bool isHolding() const { return this->holding; }

            // With start control the head is switched on by the start
            // controller; hysteresis only cuts power past its off threshold.
            void setTurnOnEnabled(bool value) { this->turnOnEnabled = value; }
        };

    }

}

#endif
