# Research: three heads on one outdoor unit

Findings from 15–18 November 2026 on the house this component was built for. They explain why the second bedroom ("Hailee's room") overshoots and power-cycles while the main floor and primary bedroom stay powered. They are the basis for [start control](../start-control-design.md) and the [supervisor](../supervisor-design.md).

Numbers come from one house over a few nights, at outdoor temperatures of about 6–11 °C. Treat them as measured on that house, not as constants.

## Contents

- [Equipment](#equipment)
- [Data and method](#data-and-method)
- [The unit's own thermostat](#the-units-own-thermostat)
- [What the unit reports](#what-the-unit-reports)
- [The shared outdoor unit leaks heat into idle heads](#the-shared-outdoor-unit-leaks-heat-into-idle-heads)
- [Main floor mornings](#main-floor-mornings)
- [Coast after a run](#coast-after-a-run)
- [What controls exist on these units, and what don't](#what-controls-exist-on-these-units-and-what-dont)
- [What was tried](#what-was-tried)
- [Conclusions](#conclusions)

## Equipment

| | Model | Notes |
|---|---|---|
| Outdoor unit | MXZ-5C42NAHZ2 | 5-port Hyper-Heat multi-split, 42k BTU |
| Main floor | SVZ-KP30NA | Ducted air handler, 30k BTU |
| Primary bedroom | MLZ-KP09NA | One-way ceiling cassette, 9k BTU |
| Second bedroom (Hailee) | MLZ-KP09NA | One-way ceiling cassette, 9k BTU; the smallest room |

Each head is driven by `climate_adaptivepid` over the CN105 port, using the [echavet/MitsubishiCN105ESPHome](https://github.com/echavet/MitsubishiCN105ESPHome) driver (pinned `cdcfc75`).

The room temperature for each bedroom is a fusion of a wall panel (75%) and a DHT22 on the head (25%), sent to the unit as its remote temperature. Both sensors sit in the head's airflow, so they read the supply-air plume during and just after a run as well as the room.

## Data and method

- **Prometheus:** Home Assistant's Prometheus exporter, 1 s–1 min resolution. It covers every diagnostic entity the component publishes (`*_operating`, `*_internal_power_on`, `*_set_point`, `*_pid_set_point`, `*_current_temperature` as the unit's view of the room, the room temperature, humidity, wall and ceiling sensors, kp/ki, model sensors). Text sensors are exported only as change counters, not values.
- **Home Assistant history export:** sub mode and stage values (text sensors), for the night of 16–17 November.
- **Analysis:** short Python scripts over those series, at 10 s resolution where timing mattered.
- **Units:** a **burst** is one period of `operating` = on. **Coast** is the peak room temperature after a burst ends, minus the room temperature when it ended. **Gap** is the device setpoint minus the unit's view of the room.

## The unit's own thermostat

The MLZ heads regulate on their own remote temperature, which the driver rounds to 0.5 °C (`round(t * 2) / 2`). Against that view, the unit behaves in three bands:

| Setpoint − unit's view | Behaviour | Evidence |
|---|---|---|
| ≥ 0 | Starts within about 10 s | 15 of 16 restarts with power already on started at a gap of 0 or +0.5 |
| −0.5 | Keeps running. A stopped unit restarts on its own once its anti-short-cycle timer allows, **6.2 min after the previous run ended** | 7 of 7 restarts with no setpoint change, all at 6.2 min |
| ≤ −1.0 | Stays off; a running unit stops within about 10 s | 24 of 26 stops (the other 2 were sampling lag) |

Power-on follows the same bands:
- **Gap of 0 or more:** all 15 hysteresis power-ons sent the target and started the unit.
- **Gap of −0.5:** a power-on started it and it ran for 3 minutes.
- **Gap of −1.0 or below:** nothing ever started, including long periods held at an 18 °C setpoint.

### Every start was triggered by the controller

On the night of 17–18 November, Hailee's room had 29 starts, all caused by setpoints the controller sent:

- **15 power-ons by hysteresis:** the device was switched on with the user's target, at or above the unit's view.
- **7 PID quantization steps:** the PID's output crossed a 0.5 rounding boundary (19.5 → 20.0), moving the gap from −0.5 to 0. Each started the compressor within 10 s, with the room only −0.22 to −0.39 below target.
- **7 timer restarts:** after a run, the setpoint was left at −0.5 (by the `run_backoff` option and floor rounding), so the unit restarted on its own 6.2 minutes later.

The PID assumes the unit's output varies smoothly with the setpoint offset (`full_output_gap`, the SIMC tuning rule). These units have only three levels. A small correction can become a full start, and the deficit at which a start happens is set by where a rounding boundary falls, not by any rule.

### PREHEAT and stage

- **PREHEAT is a fixed warm-up timer of about 7.1 minutes.** Every compressor start (power-on or restart) enters sub mode PREHEAT. It lasted 6.6–7.0 minutes on all 9 primary bedroom bursts and 7.1 minutes on all 5 main floor starts. If the compressor stops first, sub mode switches to NORMAL at the moment it stops.
- **Stage is the indoor fan speed** (the driver's own documentation), not a compressor stage.
  - **During PREHEAT it's a timer:** LOW → GENTLE at +3:00 on the bedroom units and +5:00 on the main floor unit.
  - **In NORMAL it reflects demand:** the main floor reached MODERATE and MEDIUM during a morning warm-up.
  - **After a run stops:** stage stays LOW, meaning the fan keeps running.
- **Hailee's head almost never left PREHEAT while running.** Its bursts were 3–6.8 minutes, so it was stopped mid-warm-up 22 of 23 times. Its coast grew the closer to the end of PREHEAT it stopped: restarts stopped at 5.0–5.5 minutes tripped hysteresis 1 time in 11, but at 5.7 minutes or later, 3 times in 3. The primary bedroom's bursts (8–10 minutes) completed PREHEAT and coasted only +0.19 to +0.36.

## What the unit reports

| Signal | Status on these units |
|---|---|
| `operating` (CN105 0x06 byte) | Reports this head's compressor demand. It misses heat that arrives from the shared outdoor unit: Hailee's room warmed 1.4 °C in 9 minutes, with humidity falling, while `operating` stayed 0. |
| Room temperature (unit's view) | 0.5 °C resolution, the remote temperature as rounded by the driver |
| Sub mode, stage | Available. Text in Home Assistant; numeric copies were added for Prometheus. |
| Compressor frequency, input power | **Not available on these units** |
| kWh | The main floor reads 0, and the bedroom values can't be attributed to a room |

## The shared outdoor unit leaks heat into idle heads

The MLZ-KP service manual (Note 2, "For multi system air conditioner", p.14) says: "the indoor unit that does not operate may get warm or the sound of refrigerant flowing may be heard… the refrigerant continuously flows into it."

Measured warming rate while a head is powered but its own compressor isn't running (`operating` 0 for at least 5 minutes before and 2 minutes after), split by whether another head was running (15–18 November):

| Room | Nobody else running | Another head running |
|---|---|---|
| Hailee's | −0.08 °C/min (cooling after bursts) | **+0.16 °C/min** |
| Primary | −0.03 °C/min | +0.05 °C/min |
| Main floor | 0.00 °C/min | +0.01 to +0.03 °C/min |

The room model puts Hailee's real heat loss at about 0.015 °C/min (0.084/h × an 11 °C indoor–outdoor difference). So while another head runs, the leak delivers about 10 times what her room needs. A 10-minute burst elsewhere adds about +1.6 °C.

The leak shows up the same way during bursts. Coast after Hailee's head stopped, split by whether the primary bedroom was running during the coast:

| Night | Primary running | Primary not running |
|---|---|---|
| 16–17 Nov | median +0.58 (n = 9, all ≥ +0.45) | median +0.28 (n = 15, all ≤ +0.41) |
| 17–18 Nov | median +0.83 (n = 15) | median +0.58 (n = 14) |

- **16–17 November:** every hysteresis power cut but the first coincided with the primary bedroom running.
- **17–18 November, before midnight:** only Hailee's head ran, and it stayed powered with peaks of +0.39 and no cuts. The primary bedroom's room, with its own compressor off, rose and fell about 0.2 °C in step with Hailee's bursts. At 00:13 the primary bedroom started bursts of its own, and from then on Hailee's coasts grew to +0.72–0.89.
- **17 November:** both episodes of Hailee's room heating while `operating` was 0 (20:41–20:50 and 22:05–22:12) coincided with main floor bursts.

### Only cutting power stops it

- **Lowering the setpoint doesn't.** On 17 November, holds at 18.0 (a gap of −3.5) still let the room warm about 0.1 °C/min while the main floor ran. The thermo-off fan keeps blowing at stage LOW over a coil the refrigerant keeps warm.
- **Switching the head off does.** At every power cut, the room stopped warming within 1–2 minutes and humidity recovered:
  - **In the 2 minutes before the 16 cuts on 17–18 November:** median +0.14 °C/min.
  - **In the 2 minutes after:** median −0.03 °C/min.
  - **Rise after a cut:** median +0.11 (max +0.18) on 16–17 November.

## Main floor mornings

Findings from 17–21 November 2026, at outdoor temperatures of about 2–10 °C. The main floor's schedule raises its target at 06:30, and it then runs for 66–101 minutes. The primary bedroom was not under start control, had `off_threshold` 0.75 with `off_delay` 5 min, and received no hints.

| Morning | Primary vs target when the main floor started | Primary peak | Primary's own runs | Power cut by hysteresis |
|---|---|---|---|---|
| 17 Nov | −0.05 | +0.82 | 0 | +18 min |
| 18 Nov | +0.14 | +1.00 | 0 | +26 min |
| 19 Nov | −0.20 | +1.03 | 0 | +23 min |
| 20 Nov | −0.11 | +1.17 | 0 | +24 min |
| 21 Nov | +0.06 | +0.90 | 0 | +26 min |

- **The heat came from the main floor.** The primary crossed +0.4 11–16 minutes after the main floor started.
- **The hold didn't stop it.** The setpoint held at 18.0 during `off_delay`, and the room kept warming.
- **The cut did.** After each power cut, the room rose only 0–0.08 more and then fell.

Leak into the primary while it was powered and its own head had been off for at least 10 minutes (2-minute slopes):

| Running | Median | p75 | Samples |
|---|---|---|---|
| Main floor | +0.043 °C/min | +0.062 | 233 |
| Hailee's head | +0.008 °C/min | +0.045 | 250 |
| Nothing | −0.011 °C/min | 0.000 | 675 |

So the primary needs start control's parking against the main floor. Hailee's leak into it is small, which is why the supervisor ranks the primary above Hailee's room (see [precedence](../supervisor-design.md#release-1-coordination-implemented)).

## Coast after a run

Coast is the rise in the room reading after the compressor stops: the supply-air plume clearing, the coil's stored heat, and any leak from other heads.

| Condition | Typical coast |
|---|---|
| Hailee, alone, stopped before PREHEAT's end | +0.25 to +0.4 |
| Hailee, alone, stopped near PREHEAT's end (≥ 5.7 min) | +0.45 to +0.74 |
| Hailee, another head running | +0.58 to +1.17 |
| Primary, burst completes PREHEAT | +0.17 to +0.36 |
| Main floor | +0.0 to +0.17 |

One start in Hailee's room raises it +0.8 to +1.2 °C (peak minus the room at the start). Starting at −0.3 therefore lands +0.5 to +0.9 above target.

## What controls exist on these units, and what don't

- **No installer function settings on the MLZ-KP09NA.** Its service manual lists only jumpers JR05/JR06 (remote-controller number), slide switches SW3 (airflow for ceiling height) and SWV1 (airflow direction), and timer-short mode. There is no equivalent of Mitsubishi function code 125 ("fan during heating thermo-off": extra low / stop / set speed). The thermo-off fan can't be stopped by configuration.
- **The CN105 driver** supports function reads and writes (`functions_*` entities, `hardware_settings`), but they need a unit that implements them.
- **The driver's compressor frequency with `report_when_idle`** is meant to show a shared outdoor compressor, but these units don't report frequency.
- **What the controller can actually do:** set the head's setpoint, which works in the three bands above, and switch the head off and on.

## What was tried

| Change | Effect | Outcome |
|---|---|---|
| Directional rounding (floor when heating) | Rounding never adds output | Kept; the main floor and primary bedroom use it |
| Setpoint deadband (0.1) | Stops minute-to-minute flipping at a step boundary | Kept for the PID path |
| Hysteresis `off_delay: 5min` with a hold (PID pulled to least output) | Hailee's room kept warming at about 0.1 °C/min during holds; peaks rose to +1.3 and were cut by the `off_limit` backstop | Harmful for Hailee's room (`off_delay: 0s`). Harmless where the leak is small |
| `run_backoff: 0.5` (cap the setpoint 0.5 below the unit's view at the start) | The compressor stopped at about target instead of +0.3. Coasts grew instead (median +0.72), because the stop came near PREHEAT's end and other heads kept the leak going. Left the idle setpoint at −0.5, which caused the timer restarts | Superseded by start control |
| `increase_rate: 0.1/min` | Rate-limited the continuous PID output, but the rounded setpoint still jumped a full step | Superseded by start control |
| Cutting power sooner (`off_threshold` 0.25) | Proposed: the replay put the cut about 1 minute after the stop, for an expected peak of +0.4 to +0.5 | Not deployed; start control and parking address the cause |

## Conclusions

1. **The controller, not the unit, decides every start.** The decision has to be an explicit rule matched to what a start delivers, not a side effect of rounding a continuous PID output. That is [start control](../start-control-design.md).
2. **An idle setpoint must sit at −1.0, never −0.5.** At −0.5 the unit restarts on its own timer.
3. **In Hailee's room, cutting power is the only way to stop the leak from the other heads.** So "staying continuously powered" isn't possible there while other heads cycle. The goal is the same outcome by other means:
   - **Accept the leak** when the room needs heat and another head is running.
   - **Park the head** (switch it off) before the leak overshoots, not after.
4. **Knowing that another head is running is a cross-device fact.** The supervisor knows it for certain from the other heads' `operating`. Each head can also infer it locally while idle (the room rising with humidity falling), so it keeps working without the supervisor.
