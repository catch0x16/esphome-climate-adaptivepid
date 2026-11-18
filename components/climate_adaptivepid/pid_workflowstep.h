#ifndef PID_WORKFLOWSTEP_H
#define PID_WORKFLOWSTEP_H

#include <cmath>
#include <cstdint>

#include "controller_types.h"

#include "adaptive_pid.h"
#include "burst_learner.h"
#include "room_model.h"
#include "shared_heat_detector.h"
#include "start_controller.h"
#include "tuning_rule.h"

namespace workflow {

    namespace pid {
        
        class PidWorkflowStep : public devicestate::WorkflowStep {
        private:
            AdaptivePID *adaptivePID;
            RoomModel model;
            float outdoorTemperature{NAN};
            bool holdLeast{false};
            float humidity{NAN};

            // Start control (start_controller.h): replaces the PID -> rounded
            // setpoint path when enabled.
            bool startControl{false};
            workflow::start::StartConfig startConfig{};
            workflow::start::StartController startCtl;
            workflow::start::SharedHeatDetector detector;
            workflow::start::BurstLearner bursts;
            float effectiveStartThreshold{NAN};
            workflow::start::ParkHint parkHint{workflow::start::ParkHint::NONE};
            uint32_t parkHintFromMs{0};
            uint32_t parkHintValidMs{0};

            void runStartControl(const float currentTemperature, devicestate::IDeviceStateManager* deviceManager);
            workflow::start::ParkHint currentParkHint(uint32_t now) const;
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

            bool ensurePIDTarget(devicestate::IDeviceStateManager* deviceManager);
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
            // Hold the device at the least output allowed (hysteresis is waiting
            // to switch it off); the integral is frozen meanwhile.
            void setHoldLeast(bool value) { this->holdLeast = value; }
            // Room humidity for the shared-heat detector; NaN when unknown.
            void setHumidity(float value) { this->humidity = value; }

            // Start control (docs/start-control-design.md).
            void configureStartControl(const workflow::start::StartConfig &config);
            bool isStartControl() const { return this->startControl; }
            workflow::start::HeadState startState() const { return this->startCtl.state(); }
            bool sharedHeat() const;
            float startThreshold() const { return this->effectiveStartThreshold; }
            const workflow::start::BurstState &burstState() const { return this->bursts.state(); }
            void restoreBurstState(const workflow::start::BurstState &state) { this->bursts.set_state(state); }
            void resetBurstState() { this->bursts.reset(); }
            // Supervisor hints; they expire after valid_ms.
            void setSharedHeatHint(bool active, uint32_t validMs);
            void requestPark(bool park, uint32_t validMs);
            // Start from a model state. A restored rate was learned before the
            // last reboot; a seeded one keeps the configured ki until that
            // mode's first measurement.
            void startModel(const ModelState &state, bool restoredHeat, bool restoredCool);
            // Forget everything learned and start from these seeds.
            void resetTuning(const ModelState &seeds);
        };

    }

}

#endif
