#ifndef PID_WORKFLOWSTEP_H
#define PID_WORKFLOWSTEP_H

#include <cmath>
#include <cstdint>

#include "controller_types.h"

#include "adaptive_pid.h"
#include "room_model.h"
#include "soft_start.h"
#include "tuning_rule.h"

namespace workflow {

    namespace pid {
        
        class PidWorkflowStep : public devicestate::WorkflowStep {
        private:
            AdaptivePID *adaptivePID;
            RoomModel model;
            float outdoorTemperature{NAN};
            TuningConfig tuning;

            float minTemp;
            float maxTemp;
            float maxAdjustmentOver;
            float maxAdjustmentUnder;
            float configuredKi;
            // Per mode (0 = cooling, 1 = heating): keep the configured ki until
            // that mode's first measurement.
            bool useConfiguredKi[2]{false, false};
            bool heating{true};

            SoftStart softStart;
            // For the soft start's triggers: the previous run's power and demand.
            bool hasPrevious{false};
            uint32_t previousRunMs{0};
            // Runs further apart than this are not compared (the step does not run outside HEAT/COOL).
            static constexpr uint32_t MAX_SOFT_START_GAP_MS = 30000;
            bool previousPowerOn{false};
            bool previousDemand{false};
            bool previousOperating{false};

            bool ensurePIDTarget(devicestate::IDeviceStateManager* deviceManager);
            // Arms the soft start when demand begins and measures its overshoot.
            // `maxOutput`: the most output allowed above the target; `maxOffset`:
            // how far below the target the setpoint may go. Returns the cap for
            // this run (+infinity: none).
            float updateSoftStart(const float currentTemperature, const bool demandRaised, const float maxOutput,
                                  const float maxOffset, devicestate::IDeviceStateManager* deviceManager, uint32_t now);
            void applyTunedGains(bool heating);
        
        public:
            PidWorkflowStep(
                const uint32_t updateInterval,
                const float minTemp,
                const float maxTemp,
                const float p,
                const float i,
                const float d,
                const float maxAdjustmentUnder,
                const float maxAdjustmentOver,
                const TuningConfig &tuning
            );
        
            void run(const float currentTemperature, devicestate::IDeviceStateManager* deviceManager) override;

            // Gains currently in use.
            float kp() const { return this->adaptivePID->kp(); }
            float ki() const { return this->adaptivePID->ki(); }
            float kd() const { return this->adaptivePID->kd(); }

            bool isAdaptive() const { return this->tuning.adaptive; }
            // The learned room model (room_model.h).
            const ModelState &modelState() const { return this->model.state(); }
            // Outdoor temperature for the model's heat loss; NaN when unknown.
            void setOutdoorTemperature(float value) { this->outdoorTemperature = value; }
            // Start from a model state. A restored rate was learned before the
            // last reboot; a seeded one keeps the configured ki until that
            // mode's first measurement.
            void startModel(const ModelState &state, bool restoredHeat, bool restoredCool);
            // Forget everything learned and start from these seeds.
            void resetTuning(const ModelState &seeds);

            // Soft start (soft_start.h). Can be switched on and off while running.
            void setSoftStart(const SoftStartConfig &config) { this->softStart.configure(config); }
            void setSoftStartEnabled(bool enabled);
            bool softStartEnabled() const { return this->softStart.enabled(); }
            bool softStartActive() const { return this->softStart.active(); }
            // The learned offset below the target a soft start begins at, per mode.
            float softStartOffset(bool heating) const { return this->softStart.offset(heating); }
            void setSoftStartOffset(bool heating, float value) { this->softStart.set_offset(heating, value); }
            // How far past the target the room went after the last measured soft start.
            float softStartLastOvershoot() const { return this->softStart.last_overshoot(); }
        };

    }

}

#endif
