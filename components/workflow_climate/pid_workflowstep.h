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
            // Arms the soft start when demand begins and learns the device's
            // start point. Returns the cap for this run (+infinity: none).
            float updateSoftStart(const float currentTemperature, const bool demandRaised, const float maxOutput,
                                  devicestate::IDeviceStateManager* deviceManager, uint32_t now);
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
            // The learned call at which the device starts, per mode.
            float softStartPoint(bool heating) const { return this->softStart.start_point(heating); }
            void setSoftStartPoint(bool heating, float value) { this->softStart.set_start_point(heating, value); }
        };

    }

}

#endif
