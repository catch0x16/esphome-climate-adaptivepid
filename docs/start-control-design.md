# Start control

**Status: implemented** (`pid: start_control`, `off` by default).

| Mode | What it does | Used on |
|---|---|---|
| `off` (or `false`) | The regular PID path | Main floor |
| `discrete` (or `true`) | Start control always, as described below | The second bedroom ("Hailee's room") |
| `continuous` | The regular PID path; start control only while a higher-ranked head runs (see [Continuous mode](#continuous-mode)) | The primary bedroom, since 2026-11-21, so it parks while the main floor's morning warm-up leaks into it (see [research](research/shared-outdoor-unit.md#main-floor-mornings)) |

Start control replaces "round the PID's output to the device's step and send it" with an explicit mapping. The PID's output is compared with the room temperature the way the unit compares its setpoint with it, and the matching one of three setpoint levels is sent. A state machine tracks what the unit does with that level, and switches a head off when heat is leaking in from other heads on a shared outdoor unit.

The measurements behind every rule here are in [research/shared-outdoor-unit.md](research/shared-outdoor-unit.md).

## Contents

- [Why](#why)
- [Levels](#levels)
- [States](#states)
- [Transitions](#transitions)
- [Shared heat](#shared-heat)
- [Learning](#learning)
- [Continuous mode](#continuous-mode)
- [Configuration](#configuration)
- [What it changes elsewhere](#what-it-changes-elsewhere)
- [Supervisor hints](#supervisor-hints)
- [Independence](#independence)
- [Code](#code)
- [Verification](#verification)
- [Open questions](#open-questions)

## Why

A Mitsubishi head compares its setpoint with the exact remote temperature it is sent (the room temperature, R), although it reports that temperature back rounded to its 0.5 °C step (the reported temperature):

| Setpoint | Unit |
|---|---|
| ≥ R | starts within about 20 s |
| reported − 0.5 | keeps running; a stopped unit restarts on its own after its anti-short-cycle timer (about 6.2 min) |
| reported − 1.0 | stays off; a running unit stops within about 10 s |

Across 2026-11-18/19, every start with the setpoint at or above R ran within about 20 s (15 of 15). With R 0.08–0.16 above the setpoint, the unit waited 3–4 minutes or never started (7 of 7). An earlier version started at the reported temperature, so whenever R was just above a step it stalled.

The PID assumes the unit's output varies smoothly with the setpoint, but it really has those three levels. Rounding its output onto the 0.5 grid made starts depend on where the grid fell relative to the target, not on R:

- **Accidental starts:** a correction of a few hundredths can cross a rounding boundary and start a full run.
- **Accidental restarts:** a setpoint left at −0.5 lets the unit restart on its own timer.

Heads on a shared outdoor unit add a second problem. While another head runs, an idle head that is still powered keeps receiving refrigerant, and its fan carries the heat into the room. In Hailee's room that is +0.16 °C/min. Lowering the setpoint doesn't stop it; switching the head off does.

## Levels

Every update (2 s), the PID's output O (the setpoint it would send if the unit took any value, bounded to target − `max_adjustment_under` … target + `max_adjustment_over`) is compared with R. When cooling, every sign is mirrored.

| O compared with R | Level | Setpoint sent |
|---|---|---|
| O ≥ R | start | the 0.5 step at or above R (20.08 → 20.5, 20.00 → 20.0) |
| R − 0.5 ≤ O < R | running | reported − 0.5 |
| O < R − 0.5 | off | reported − 1.0 |

The 0.5 is `stop_band` (one step). A level is left for a lower one only once O is 0.05 past the boundary (`level_deadband`), so noise on R doesn't flip it: a start is dropped at O < R − 0.05, and a run stopped at O < R − 0.55.

With the integral at zero, O − R = (1 + kp)(target − R). With kp = 0.75 a run starts once the room reaches the target, and holds the running level until the room is about 0.3 past it. The integral then moves both points: a room that averages past the target pulls them down, one that averages short pushes them up.

## States

| State | Device | Setpoint | Meaning |
|---|---|---|---|
| IDLE | on | off level; the running level while the PID asks for it and nothing holds the head back | Not running |
| START | on | the level asked for (start level) | A start was asked for; the run isn't confirmed yet |
| RUN | on | start or running level (the running level is held within `min_run`) | Running |
| PARKED | off | — (the off level is used to switch on) | Blocks heat leaking in from other heads |

A step is the device's setpoint step (0.5 on these units).

## Transitions

Evaluated on every workflow update (2 s) while the outer mode is HEAT or COOL.

| From → to | Trigger | Action |
|---|---|---|
| IDLE → START | start level, **and** no shared heat, **and** ≥ `min_cycle_duration` since the last stop, **and** hysteresis isn't holding, **and** not deferred (below) | start level |
| IDLE (deferred) | a start is due, **but** the supervisor forecasts shared heat within 5 min, **and** the room is less than 0.5 short of the target, **and** the deferral has lasted less than 6 min | off level. Ends when the heat arrives, the forecast is dropped, at the 0.5 floor, or after 6 min |
| START → RUN | the unit has reported `operating` for 60 s without a break (right after power-on a unit can report running for ~20 s and stop; until confirmed the start is kept) | level asked for |
| START → IDLE | the PID no longer asks for a start and the unit never ran | no stop is counted, so the minimum cycle doesn't apply |
| START (fault) | 15 min with no run (`start_fault_ms`) | logged once at WARNING; the start is kept |
| START → IDLE | shared heat starts | off level |
| RUN → IDLE | the unit stops by itself | — |
| RUN → IDLE (PID stop) | off level, **and** at least 2 minutes since the start (`min_run`) | off level (the unit stops within 10 s) |
| RUN → IDLE | shared heat starts | off level |
| RUN → IDLE | at least 2 minutes running, **and** shared heat forecast within 2 min | off level |
| IDLE → RUN | the unit runs without a START, more than 60 s after the last commanded stop (for example on its own timer at the running level) | tracked as a run (logged as unrequested) |
| START/RUN → IDLE | hysteresis is holding (room past `off_threshold`, waiting out `off_delay`) | off level |
| IDLE → PARKED | shared heat **and** room > target + `off_threshold`, **and** ≥ `min_cycle_duration` since the last power change | switch the device off (at once, no `off_delay`) |
| any → PARKED | the device is off (parked, or cut by hysteresis after `off_delay` / past `off_limit`) | — |
| PARKED → IDLE | room < target − `on_threshold`, **and** ≥ `min_cycle_duration` since the last power change | switch the device on at the off level, never the target |
| PARKED → IDLE (catch) | room more than 0.1 short of the target, **and** shared heat arriving or forecast within 5 min, **and** ≥ `min_cycle_duration` since the last power change | switch the device on, so its fan brings in the other head's heat |

In IDLE, the off level is also sent within 60 s (`stop_grace`) of a commanded stop, and while shared heat is on or forecast, so the unit isn't left free to restart on its own timer.

**The integral** accumulates in IDLE, START and RUN, so it follows the room through whole cycles. It is frozen only while PARKED. While PARKED the PID keeps running (unlike the regular path, which resets it when the device is off), so its output is current once the head is switched back on. It resets when the target changes, and stops accumulating at either output bound.

**Power follows the hysteresis band.** Parking and unparking use the hysteresis settings, so start control has no thresholds of its own for power:
- **Inside the band** (between target − `on_threshold` and target + `off_threshold`) the head stays powered. While another head runs it takes the leak instead of switching off and on for every small rise.
- **Past `off_threshold`** it is parked. With shared heat at once: the leak won't stop while the head is powered, so waiting out `off_delay` only adds overshoot. Without shared heat the hysteresis step cuts it after `off_delay` (or at once past `off_limit`), stopping any run while it holds, as on the regular path.
- **Back on at `on_threshold`.** A parked head can't see shared heat (its fan is off), so it is switched back on, idle, on the room alone. Then IDLE → START happens as usual, or the leak warms it again if another head is still running.

The band width is the trade between comfort and power cycles. On 18 November a band of about 0.2 (park within 0.2 of the target, unpark on demand) held Hailee's room within −0.16…+0.18 with no runs of its own, at about 1.6 power cycles an hour, each powered stretch lasting 1–3 minutes.

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
| Gain (alone, shared) | peak − room at start | logged only | — |
| Coast (alone, shared) | peak − room at stop | logged only | 0.3 alone, 0.6 shared |
| Coast time | peak time − stop time | detector eligibility | 5 min |

Gain and coast no longer drive any decision: the PID's output decides when to start and stop. They are still learned and logged, as a record of how each run behaved.

- **Smoothing:** each sample moves the estimate 30% of the way.
- **Minimum data:** a learned value is used after 3 runs.
- **Discarded runs:** a run whose coast was cut short (device switched off, a new start) is not recorded, because it would understate the coast.
- **Persistence:** saved in its own preference (`BURSTS_PREFERENCE_TYPE`) on the same schedule as the room model and on safe shutdown. `reset_tuning()` clears it.

## Continuous mode

**Why.** Some heads need start control only while another head leaks heat into them. The primary bedroom shows it:
- **Its own cycles are fine** on the regular path, with a 0.75 band and `off_delay` 5 min.
- **The main floor's morning runs** warm it about 0.04 °C/min with no run of its own. Holding the setpoint during `off_delay` doesn't stop that, so it peaked at +0.8…+1.2.
- **Always-on start control** would fix the mornings, but it would also change every cycle of its own. Its own coasts would go past a tight +0.4 band about 5 times a night.

**What it does.** The head runs the regular PID path, with its own hysteresis band, until start control engages. While engaged, it behaves exactly as in discrete mode: no runs of its own, park at once past `shared_off_threshold`, switch back on to catch the other head's heat.

| | Rule |
|---|---|
| **Engages** | The supervisor's `shared_heat` hint is on, or it forecasts a start within 5 min. The supervisor sends a room only the heads that rank above it (see [precedence](supervisor-design.md#release-1-coordination-implemented)). The local detector doesn't count: that leak can be below what it detects, and a false trigger would change the head's behaviour for nothing. |
| **On entry** | The state is adopted from the device: off is PARKED, running is RUN, otherwise IDLE. The last stop and power change seen on the regular path carry over, so `min_cycle_duration` still holds. A run of its own is stopped at once by the shared heat; the other head's heat replaces it. The integral carries over. |
| **Released** | Neither signal for `min_cycle_duration`, **and** IDLE or PARKED, so a run it started finishes first. |
| **After release** | The regular path again. A parked head stays off until the hysteresis switches it on at `on_threshold`, as after a hysteresis cut. |

**Rules while engaged:**
- **Parking** happens at once past `hysteresis: shared_off_threshold` (default `off_threshold`). The regular path keeps `off_threshold`, so the head's own band doesn't change.
- **Hysteresis never switches the head on**, as in discrete mode.
- **Runs aren't learned.** Start control drives the head only in stretches, so its coasts would be cut short.

```yaml
    pid:
      start_control: continuous
    hysteresis:
      off_threshold: 0.75              # the head's own band, as on the regular path
      off_delay: 5min
      shared_off_threshold: 0.4        # parked at once past this while a higher-ranked head runs
```

**Diagnostics:** `Control state` is 4 (CONTINUOUS) while not engaged. Engaging and release are logged at INFO.

## Configuration

```yaml
climate:
  - platform: climate_adaptivepid
    humidity_sensor: dht_humidity      # optional; confirms shared heat
    pid:
      max_adjustment_under: 1.0        # the PID's output stays within target - 1.0 ...
      max_adjustment_over: 0.5         # ... target + 0.5
      start_control: discrete          # true also works
    hysteresis:                        # required with start_control
      on_threshold: 0.25               # a parked head is switched back on (idle) this far below target
      off_threshold: 0.4               # parked this far past target (at once with shared heat)
      off_delay: 5min                  # optional; only without shared heat
      off_limit: 0.75                  # optional; cut at once past this
      min_cycle_duration: 60s          # also the minimum between starts and power changes
```

Validation rejects:
- **`start_threshold`, `start_timeout` and the `start_threshold_sensor`:** removed; the PID's output decides when a run starts and how long a start is held.
- **`start_control` without `hysteresis`**, in either mode.
- **`shared_off_threshold` without `start_control: continuous`:** discrete mode parks at `off_threshold`.
- **`park_when_shared`, `park_margin` and the `leak_rate` sensor:** removed; parking follows the hysteresis band.

**Diagnostics** (created only with start control):
- **`Control state`:** 0 IDLE, 1 START, 2 RUN, 3 PARKED, 4 CONTINUOUS (continuous mode, not engaged).
- **`Shared heat`.**
- **`PID Set Point`** shows the PID's output (O, continuous); `Set Point` shows the level sent to the unit.

Every transition is logged at INFO with room, target, the PID's output, the level, the setpoint sent, the reported temperature, and the shared-heat source.

## What it changes elsewhere

| Setting | With start control |
|---|---|
| `setpoint_deadband`, directional rounding | Not used: the three levels are already on the grid (`internalSetDeviceTemperature` rounds to nearest, without a deadband); `level_deadband` keeps the level from flipping instead. Still used by rooms without start control. |
| Hysteresis `on_threshold` | Where the start controller switches a parked head back on (idle). Hysteresis itself never switches the head on. |
| Hysteresis `off_threshold` | Where the head is parked: at once with shared heat (start controller), otherwise by the hysteresis step after `off_delay`. A cut counts as PARKED. |
| Hysteresis `off_delay` / `off_limit` (hold) | Apply without shared heat; while holding, a run is stopped and none is started. |
| Switching to HEAT/COOL | The device is switched on at the off level, not the target. |
| Reboot with HEAT and the device off | Left off. The start controller switches it on at `on_threshold`. |
| `run_backoff`, `increase_rate` | Removed: RUN replaces `run_backoff`, and `increase_rate` shaped a continuous setpoint that is no longer sent. |

## Supervisor hints

The climate exposes one method, called from an ESPHome action in the device YAML. It expires on the head.

| Method | Effect |
|---|---|
| `set_shared_heat_hint(active, valid_for_s)` | Replaces the local shared-heat detector while valid |
| `set_shared_heat_expected(expected_in_s, valid_for_s)` | Forecast: another head is predicted to start in `expected_in_s` (counted down on the head; negative clears it). Defers starts, switches a parked head on to catch the heat, and stops a run early (see [Transitions](#transitions)). There is no local forecast |

The head makes every power decision itself; the hints only tell it, for certain, whether another head is running, and when one is expected to. The [`climate_adaptivepid_supervisor`](../custom_components/climate_adaptivepid_supervisor) integration sends it; see [supervisor-design.md](supervisor-design.md).

## Independence

| Situation | Behaviour |
|---|---|
| Supervisor stopped or disabled | Its hint expires within 2 minutes; the local detector takes over |
| Home Assistant unavailable | As above. The room temperature falls back to the unit's own sensor after `ha_temperature_timeout_minutes` (device YAML) |
| Unit reports no temperature | The room temperature, rounded to the step, stands in for the reported temperature |
| `start_control: false` | Exactly the PID path as before |

## Code

| File | Role |
|---|---|
| `start_controller.h` | Levels and state machine (pure, tested natively) |
| `shared_heat_detector.h` | Local detector and hint expiry (pure) |
| `burst_learner.h` | Gain and coast learning (pure); coast time for the detector, the rest logged |
| `start_engagement.h` | Continuous mode: when start control engages and is released (pure) |
| `pid_workflowstep.cpp` (`runStartControl`, `runEngagement`) | Wiring: the PID's output, detector, learner and controller; applies power and setpoints; engaging and releasing in continuous mode |
| `climate_device_adapter.cpp` | `internalTurnOnAt` (switch on at a given setpoint) and `internalSetDeviceTemperature` (exact level, no deadband) |
| `hysterisis_workflowstep.*` | The cut after `off_delay` and the hold; `setTurnOnEnabled(false)` under start control |
| `climate_adaptivepid.*`, `climate.py` | Options, diagnostics, persistence, hint methods, power-on at the idle setpoint |

## Verification

- **Native tests** (`uv run python tests/run_native.py`):
  - **`test_start_controller.cpp`:** the level at the O − R boundaries and the deadband, the start level from R (20.08 → 20.5, 19.92 → 20.0, 20.00 → 20.0), the running level, the PID stop only after `min_run`, the off level within `stop_grace`, a restart at the running level tracked as a run, the minimum cycle, no start with shared heat or while hysteresis holds, run confirmation, the start fault, deferral, catch and stopping for expected heat, staying powered inside the band, parking past `off_threshold` on shared heat and unparking past `on_threshold` with throttling, a hold stopping a run or start, the integral frozen only while parked, and cooling.
  - **`test_shared_heat_detector.cpp`:** detection with and without humidity, rejection when humidity is steady, eligibility, cooling, and hint expiry.
  - **`test_burst_learner.cpp`:** gain and coast alone vs shared, discarding coasts cut short and starts that never ran, and restoring state.
  - **`test_start_engagement.cpp`:** engaging on a hint or a forecast within the window, holding through a short gap, releasing only after `min_cycle_duration` and only once settled, and time wrapping around. `test_start_controller.cpp` also covers `resync`: the state adopted from the device, the minimum cycle across the change, and catching heat while parked.
- **Host scenarios:**
  - **`tests/fixtures/start_control.yaml` (discrete):** idle power-on, staying powered on shared heat inside the band, parking at once past `off_threshold` on shared heat, unparking at `on_threshold`, and the hysteresis cut after `off_delay` without shared heat.
  - **`tests/fixtures/start_control_continuous.yaml`:** the regular path with no hint, then engaging and parking at once past `shared_off_threshold` (inside the 0.75 band), released after `min_cycle_duration`, and switched back on by the hysteresis at `on_threshold`.
- **Live checks for Hailee's room**, against 2026-11-18/19:
  - every start level is followed by `operating` within about 20 s,
  - the share of the night within ±0.25, above +0.5 and below −0.5, and the warmest and coldest,
  - runs ended by the PID against runs the unit ended itself,
  - kp and the integral settle rather than sitting on a bound.

## Open questions

- **Peaks while the integral learns:** with the integral near zero a run starts at the target and holds to about +0.3, then coasts. Until the integral pulls the start in (ki ≈ 0.00036/s on Hailee's room, about an hour per 0.4 °C), peaks can run higher than with the old learned coast stop.
- **Adaptive kp:** the retune can move kp between 0.25 and 4. The higher it is, the closer the start and stop points, and the more the cycle length rests on `min_run` and `min_cycle_duration`.
- **The running level while stopped:** in IDLE the running level lets the unit restart on its own timer when the PID asks for a little heat. Whether that helps or just adds short runs needs a night of data.
- **Cooling:** the bands are measured for heating. Cooling is mirrored by assumption.
