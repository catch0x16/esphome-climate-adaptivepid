# Start control

**Status: implemented** (`pid: start_control`, off by default). Enabled on the second bedroom ("Hailee's room").

Start control replaces "round the PID's output to the device's step and send it" with an explicit state machine. The PID supplies demand; the state machine decides when the unit starts and stops, and when to switch a head off because heat is leaking in from other heads on a shared outdoor unit.

The measurements behind every rule here are in [research/shared-outdoor-unit.md](research/shared-outdoor-unit.md).

## Contents

- [Why](#why)
- [States](#states)
- [Transitions](#transitions)
- [Shared heat](#shared-heat)
- [Learning](#learning)
- [Configuration](#configuration)
- [What it changes elsewhere](#what-it-changes-elsewhere)
- [Supervisor hints](#supervisor-hints)
- [Independence](#independence)
- [Code](#code)
- [Verification](#verification)
- [Open questions](#open-questions)

## Why

A Mitsubishi head compares its setpoint with its own view of the room, the remote temperature rounded to 0.5 °C:

| Setpoint − view | Unit |
|---|---|
| ≥ 0 | starts within about 10 s |
| −0.5 | keeps running; a stopped unit restarts on its own after its anti-short-cycle timer (about 6.2 min) |
| ≤ −1.0 | stays off; a running unit stops within about 10 s |

The PID assumes the unit's output varies smoothly with the setpoint offset, but it really has three levels. Rounding its output onto that grid has two effects:

- **Accidental starts:** a correction of a few hundredths can cross a rounding boundary and start a full run. Where the boundary falls depends on the integral, kp and the target's position on the grid, not on any rule.
- **Accidental restarts:** an idle setpoint left at −0.5 lets the unit restart on its own timer.

In a small room, a start adds +0.8 to +1.2 °C, so starts at −0.2 to −0.4 overshoot every time.

Heads on a shared outdoor unit add a second problem. While another head runs, an idle head that is still powered keeps receiving refrigerant, and its fan carries the heat into the room. In Hailee's room that is +0.16 °C/min. Lowering the setpoint doesn't stop it; switching the head off does.

## States

The setpoint is always relative to the unit's view. When cooling, every sign is mirrored.

| State | Device | Setpoint | Purpose |
|---|---|---|---|
| IDLE | on | view − 2 steps (−1.0) | Off, and can't restart on its timer |
| START | on | view | Start now |
| RUN | on | view at start − 1 step (−0.5) | Lowest demand; the unit stops by itself as soon as its view moves one step |
| PARKED | off | — (the IDLE level is used to switch on) | Blocks heat leaking in from other heads |

A step is the device's setpoint step (0.5 on these units).

## Transitions

Evaluated on every workflow update (2 s) while the outer mode is HEAT or COOL.

| From → to | Trigger | Action |
|---|---|---|
| IDLE → START | demand ≥ `start_threshold`, **and** no shared heat, **and** the room isn't rising (≥ 0.02 °C/min over 2 min), **and** ≥ `min_cycle_duration` since the last stop, **and** no park request | setpoint = view |
| START → RUN | the unit reports `operating` | setpoint = view at start − 1 step |
| START → IDLE | no `operating` within 60 s | idle setpoint |
| RUN → IDLE | the unit stops by itself | idle setpoint |
| RUN → IDLE (early stop) | at least 2 minutes running, **and** room + learned coast ≥ target | idle setpoint (the unit stops within 10 s) |
| RUN → IDLE | shared heat starts | idle setpoint |
| IDLE → RUN | the unit runs without a START, more than 60 s after the last commanded stop | tracked as a run (logged as unrequested) |
| IDLE → PARKED | `park_when_shared`, **and** (shared heat **and** room ≥ target − `park_margin`, **or** a supervisor park request), **and** ≥ `min_cycle_duration` since the last power change | switch the device off |
| any → PARKED | the device is off (parked, or cut by the hysteresis backstop) | — |
| PARKED → IDLE | demand ≥ `start_threshold` − 0.2 (≥ `start_threshold` while a park request is active), **or** a supervisor unpark request | switch the device on at the idle setpoint, never the target |

**Demand** is the PID's output minus the target (target minus output when cooling): how much more output the PID asks for. Because it includes the integral, a room that has been below target for a long time starts sooner.

**The integral** accumulates only while IDLE. It is frozen in START, RUN and PARKED, while the controller holds the output below what the PID asks for. While PARKED the PID keeps running, unlike the regular path, which resets it when the device is off, so demand can unpark the head.

**Leaving PARKED.** A parked head can't see shared heat: its fan is off, so nothing reaches the sensors. Unparking therefore follows the room's own demand. Unparking slightly below the start threshold (−0.2) gives the head a chance to take leaking heat before it needs a run of its own:
- **Another head is still running:** the room warms from the leak, and the head parks again near target.
- **Nothing else is running:** IDLE → START happens as usual.

## Shared heat

"Shared heat" means heat (cooling) is arriving from other heads on the outdoor unit.

**Local detector** (`shared_heat_detector.h`). It is on when all of these hold:
- the head is powered, IDLE, not in a coast it is learning, and past its learned coast time since its own last stop (5 minutes by default),
- the room has moved the way output would move it by at least 0.05 °C/min over the last 2 minutes,
- humidity has fallen at least 0.2 % over those 2 minutes (heating only, when `humidity_sensor` is set).

It turns off once the room has stopped moving (≤ 0.01 °C/min over 2 minutes), or when the head is no longer eligible. It keeps about 4 minutes of history at 10 s per sample.

**Supervisor hint.** While valid, it replaces the local detector. When it expires, the detector decides again.

**The units report no compressor frequency or input power**, so there is no direct signal on the head.

## Learning

`burst_learner.h` learns from each run: from the START decision (or an unrequested run), through the stop, to the coast's peak. The coast ends 0.1 °C after the peak, or 15 minutes after the stop. Each run is filed under "alone" or "shared": shared if shared heat was seen at any point.

| Value | Measured as | Used for | Default |
|---|---|---|---|
| Gain (alone) | peak − room at start | `start_threshold` = kp × gain / 2 (clamped to 0.2…3.0, and below `max_adjustment_over`) | the configured `start_threshold` |
| Coast (alone, shared) | peak − room at stop | early stop: room + coast ≥ target | 0.3 alone, 0.6 shared |
| Coast time | peak time − stop time | detector eligibility | 5 min |
| Leak rate | room change ÷ time over a shared-heat episode of at least 2 minutes | `park_margin` = leak × 2 min (clamped to 0.1…0.6) | the configured `park_margin` |

- **Smoothing:** each sample moves the estimate 30% of the way.
- **Minimum data:** a learned value is used after 3 runs (2 episodes for the leak rate).
- **Discarded runs:** a run whose coast was cut short (device switched off, a new start) is not recorded, because it would understate the coast.
- **Persistence:** saved in its own preference (`BURSTS_PREFERENCE_TYPE`) on the same schedule as the room model and on safe shutdown. `reset_tuning()` clears it.

## Configuration

```yaml
climate:
  - platform: climate_adaptivepid
    humidity_sensor: dht_humidity      # optional; confirms shared heat
    pid:
      max_adjustment_over: 1.0         # must exceed start_threshold
      start_control: true
      start_threshold: 0.5             # PID demand that starts a run, C
      park_when_shared: true           # requires start_control
      park_margin: 0.2                 # park within this of the target, C
    hysteresis:
      off_threshold: 0.75              # backstop cut
      min_cycle_duration: 60s          # also the minimum between starts and power changes
```

Validation rejects:
- **`start_threshold` at or above `max_adjustment_over`:** demand is capped there, so the unit could never start.
- **`park_when_shared` without `start_control`.**

**Diagnostics** (created only with start control):
- **`Control state`:** 0 IDLE, 1 START, 2 RUN, 3 PARKED.
- **`Start threshold`:** the one in use, learned or configured.
- **`Leak rate`:** in °C/h.
- **`Shared heat`.**

Every transition is logged at INFO with room, target, demand, the threshold, the unit's view, and the shared-heat source.

## What it changes elsewhere

| Setting | With start control |
|---|---|
| `setpoint_deadband`, directional rounding | Not used: the three levels are already on the grid (`internalSetDeviceTemperature` rounds to nearest, without a deadband). Still used by rooms without start control. |
| Hysteresis `on_threshold` | Ignored: hysteresis never switches the head on. |
| Hysteresis `off_threshold` / `off_limit` | The backstop. A cut counts as PARKED. |
| Hysteresis `off_delay` (hold) | Ignored (no hold). |
| Switching to HEAT/COOL | The device is switched on at the idle setpoint, not the target. |
| Reboot with HEAT and the device off | Left off. The start controller switches it on when demand calls for it. |
| `run_backoff`, `increase_rate` | Removed: RUN replaces `run_backoff`, and `increase_rate` shaped a continuous setpoint that is no longer sent. |

## Supervisor hints

The climate exposes two methods, called from ESPHome actions in the device YAML. Both expire on the head.

| Method | Effect |
|---|---|
| `set_shared_heat_hint(active, valid_for_s)` | Replaces the local shared-heat detector while valid |
| `request_park(park, valid_for_s)` | Park (only with `park_when_shared`) or unpark while valid; a park request also raises the unpark threshold to `start_threshold` |

The [`climate_adaptivepid_supervisor`](../custom_components/climate_adaptivepid_supervisor) integration sends them; see [supervisor-design.md](supervisor-design.md).

## Independence

| Situation | Behaviour |
|---|---|
| Supervisor stopped or disabled | Its hints expire within 2–3 minutes; the local detector and the configured or learned thresholds take over |
| Home Assistant unavailable | As above. The room temperature falls back to the unit's own sensor after `ha_temperature_timeout_minutes` (device YAML) |
| Unit reports no temperature | The room temperature, rounded to the step, stands in for the unit's view |
| `start_control: false` | Exactly the PID path as before |

## Code

| File | Role |
|---|---|
| `start_controller.h` | State machine (pure, tested natively) |
| `shared_heat_detector.h` | Local detector and hint expiry (pure) |
| `burst_learner.h` | Gain, coast and leak learning (pure) |
| `pid_workflowstep.cpp` (`runStartControl`) | Wiring: PID demand, detector, learner and controller; applies power and setpoints |
| `climate_device_adapter.cpp` | `internalTurnOnAt` (switch on at a given setpoint) and `internalSetDeviceTemperature` (exact level, no deadband) |
| `hysterisis_workflowstep.*` | `setTurnOnEnabled(false)` under start control |
| `climate_adaptivepid.*`, `climate.py` | Options, diagnostics, persistence, hint methods, power-on at the idle setpoint |

## Verification

- **Native tests** (`uv run python tests/run_native.py`):
  - **`test_start_controller.cpp`:** the three levels, IDLE after every stop, starts on demand only (not on a small deficit, mid-coast, with shared heat, or under a park request), the minimum cycle, early stop after the minimum run, a lagging `operating` report not counted as an unrequested start, park and unpark with throttling, the hysteresis cut as PARKED, supervisor hints, and cooling.
  - **`test_shared_heat_detector.cpp`:** detection with and without humidity, rejection when humidity is steady, eligibility, cooling, and hint expiry.
  - **`test_burst_learner.cpp`:** gain and coast alone vs shared, discarding coasts cut short and starts that never ran, the leak rate, and restoring state.
- **Host scenario:** `tests/fixtures/start_control.yaml`, a smoke test of idle power-on, supervisor park and unpark, the backstop as PARKED, and unparking on demand.
- **Live checks for Hailee's room**, against 16–18 November:
  - every start is preceded by a START log line,
  - no timer restarts at −0.5,
  - median peak at most +0.4,
  - hysteresis cuts near 0,
  - parks happen before peaks while other heads run.

## Open questions

- **Early stop timing:** the minimum run before an early stop (2 minutes) and the shared-coast default are guesses until a few nights are learned.
- **Learning start against stop:** with the unit stopping itself at a one-step rise, the learned gain mostly reflects where the unit's view crosses a 0.5 boundary. If peaks stay high, the next lever is to start later: a lower demand cap, or `start_threshold` from the *shared* gain when other heads are known to be running.
- **Cooling:** the bands are measured for heating. Cooling is mirrored by assumption.
