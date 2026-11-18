#pragma once

#include <cmath>
#include <cstdint>

// When hysteresis switches a running device off. No ESPHome dependency, so it
// can be tested natively (tests/test_off_gate.cpp).
//
// The room sensors sit in the device's warm (or cold) air, so the room reading
// keeps running past the target for a few minutes after the compressor stops,
// then comes back on its own. Switching the device off on that brief peak means
// a full power-on start next time, which runs harder and overshoots further.
// So a reading past the off threshold first only holds: the PID is pulled to
// its least output while the room settles. The device is switched off once the
// reading has stayed past the threshold for `delay_ms`, or at once past
// `limit`, the backstop against a room that is really overheating.
//
// delta: how far the room is past the target in the direction of the output,
// e.g. room - target while heating.
namespace workflow {
namespace hysterisis {

enum class OffAction {
  NONE,      // below the off threshold
  HOLD,      // past the threshold: pull the output down and wait
  TURN_OFF,  // past the threshold for the delay, or past the limit
};

class OffGate {
 public:
  OffGate() = default;
  OffGate(float threshold, float limit, uint32_t delay_ms)
      : threshold_(threshold), limit_(limit), delay_ms_(delay_ms) {}

  OffAction update(float delta, uint32_t now_ms) {
    if (!std::isfinite(delta) || !(delta > this->threshold_)) {
      this->pending_ = false;
      return OffAction::NONE;
    }
    if (std::isfinite(this->limit_) && delta > this->limit_) {
      return OffAction::TURN_OFF;
    }
    if (!this->pending_) {
      this->pending_ = true;
      this->since_ms_ = now_ms;
    }
    return now_ms - this->since_ms_ >= this->delay_ms_ ? OffAction::TURN_OFF : OffAction::HOLD;
  }

  // The device is off (or the mode changed): start afresh next time.
  void reset() { this->pending_ = false; }

  // How long the reading has been past the threshold, 0 when it is not.
  uint32_t held_ms(uint32_t now_ms) const { return this->pending_ ? now_ms - this->since_ms_ : 0; }

 private:
  float threshold_{0.25f};
  float limit_{NAN};
  uint32_t delay_ms_{0};
  bool pending_{false};
  uint32_t since_ms_{0};
};

}  // namespace hysterisis
}  // namespace workflow
