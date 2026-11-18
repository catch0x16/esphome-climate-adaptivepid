# ESPHome Adaptive PID Climate

`climate_adaptivepid` is a climate that wraps another ESPHome climate and runs a
PID controller and a hysteresis (on/off) controller on top of it, entirely on
the device.

It keeps two states apart:

- **What the user sees**: the outer climate's mode and target, e.g. `HEAT, 21°C`.
- **What the device is told**: the inner climate may be switched off while the
  outer one still reads `HEAT`, and its setpoint is the PID-corrected value
  (e.g. `22.5°C`) rather than the user's target.

The wrapper only uses the generic ESPHome climate API, so it works with any
single-target climate. It was written for Mitsubishi heat pumps driven by
[echavet/MitsubishiCN105ESPHome](https://github.com/echavet/MitsubishiCN105ESPHome).

## Configuration

```yaml
external_components:
  - source: github://catch0x16/esphome-mitsubishiheatpump
    components: [climate_adaptivepid]
  - source: github://echavet/MitsubishiCN105ESPHome
    components: [cn105]

uart:
  id: hp_uart
  baud_rate: 2400
  tx_pin: 17
  rx_pin: 16

climate:
  # The device driver. Hidden, so Home Assistant only sees the adaptive PID climate.
  - platform: cn105
    id: hp
    name: "Heat pump device"
    internal: true
    uart_id: hp_uart
    update_interval: 2s
    remote_temperature_source:
      sensor_id: room_temperature
    supports:
      mode: [COOL, HEAT, FAN_ONLY, DRY]
      # dual_setpoint must stay off (the default)

  - platform: climate_adaptivepid
    name: "HVAC"
    climate_id: hp
    temperature_sensor: room_temperature
    update_interval: 2s
    pid:
      kp: 1.5
      ki: 0.015
      kd: 0.0
      max_adjustment_under: 2.0
      max_adjustment_over: 2.0
    hysteresis:
      on_threshold: 0.25
      off_threshold: 0.5
      off_delay: 5min    # optional
      off_limit: 1.0     # optional
      min_cycle_duration: 60s
```

| Option | Default | Meaning |
|---|---|---|
| `climate_id` | required | The inner climate to drive. |
| `temperature_sensor` | inner climate's temperature | Room temperature used by the workflows and shown as the current temperature. |
| `update_interval` | `2s` | How often the workflows run. |
| `settle_time` | `2 × update_interval + 5s` | How long after a power command the inner climate's reported power is ignored. |
| `ready_sensor` | none | Optional binary sensor; while it is off the workflows are paused. |
| `outdoor_temperature_sensor` | none | Outdoor temperature for the room model's heat loss. Without it heat loss is not learned and is left out of the heating and cooling rates. |
| `humidity_sensor` | none | Room humidity. With start control it confirms heat arriving from other heads on a shared outdoor unit (warm supply air dries the room). |
| `pid` | disabled | `kp`, `ki`, `kd`, and how far the device setpoint may move below/above the target (`max_adjustment_under` / `max_adjustment_over`), and `setpoint_deadband`. See [PID gains](#pid-gains). Start control: `start_control`, `start_threshold`; see [Start control](#start-control). |
| `hysteresis` | disabled | Switch the device off once the room is `off_threshold` past the target and back on once it is `on_threshold` short of it, at most once per `min_cycle_duration`. With `off_delay` (default `0s`) a room past `off_threshold` is first held: the PID is pulled to its least output (target minus `max_adjustment_under` while heating) with its integral frozen, and the device is switched off only if the room is still past the threshold after `off_delay`, or at once past `off_limit` (default `off_threshold` + 0.5). Room sensors near the unit read past the target for a few minutes after each run; the delay keeps those brief peaks from forcing a full power-on start. |

At least one of `pid` and `hysteresis` is required. The standard climate
options (`name`, `visual`, ...) apply; the visual range defaults to the inner
climate's and can only be narrowed.

Diagnostic entities are created automatically: `Internal power on` (what the
workflow last commanded), `Device power on`, `Operating`, `Current temperature`
(the device's own reading), `Set Point` (the device's setpoint) and
`PID Set Point`, plus `PID Kp` and `PID Ki` when the PID is used, and with
adaptive tuning the learned room model: `Heating rate`, `Cooling rate`,
`Heat loss`, `Dead time heating` and `Dead time cooling`. Each can be renamed
or reconfigured under its key (`internal_power`, `device_power`, `operating`,
`device_current_temperature`, `device_set_point`, `pid_set_point`, `pid_kp`,
`pid_ki`, `heating_rate`, `cooling_rate`, `heat_loss`, `dead_time_heating`,
`dead_time_cooling`). With start control there are also `Control state`
(0 IDLE, 1 START, 2 RUN, 3 PARKED), `Start threshold` and `Shared heat`
(`control_state`, `start_threshold_sensor`, `shared_heat`). Anything device-specific, such as
compressor frequency or energy, comes from the inner climate's own sensors.

### PID gains

The PID output is a setpoint offset in degrees, added to the target:

```
offset = kp * error + ki * integral(error) + kd * d(error)/dt      error = target - room
```

- `kp`: degrees of setpoint per degree of error. `kp: 1.0` asks for a setpoint
  1°C above the target when the room is 1°C below it.
- `ki`: degrees of setpoint per degree-second of accumulated error. `ki: 0.001`
  adds 0.06°C per minute for each degree of error.
- `kd`: degrees of setpoint per degree-per-second of change.

The offset is limited to `-max_adjustment_under .. +max_adjustment_over` when
heating (mirrored when cooling) and then rounded to the device's step, towards
less output: down when heating, up when cooling. Rounding therefore never asks
for more heating or cooling than the PID did; the integral makes up the
shortfall over time. The user's own target, when no correction applies, is
rounded to nearest.

`setpoint_deadband` (default 0.1°C) stops the device setpoint flipping between
two steps while the correction hovers at a step boundary: a step towards more
output is taken only once the correction is that far past the boundary, a step
towards less output at once. On a 0.5°C device held at 21.0 while heating, the
setpoint moves to 21.5 once the correction reaches 21.6, and back to 21.0 as
soon as it falls below 21.5. `0` disables it. The rounded value never exceeds those limits, so a limit
should be a multiple of the step: on a 0.5°C device `max_adjustment_over: 0.25`
allows no upward move. A warning is logged at startup when a limit is not a
multiple of the step. With a target between steps (67°F is 19.44°C) the usable
range is narrower still: `max_adjustment_over: 0.5` reaches 19.5 at most.
The integral stops accumulating while the offset is at a limit, and it is
reset on a target change and while the device is switched off.

### Start control

For units whose own thermostat has only three levels, and for heads that share
an outdoor unit. Off by default; the full design is in
[docs/start-control-design.md](docs/start-control-design.md), and the
measurements behind it in
[docs/research/shared-outdoor-unit.md](docs/research/shared-outdoor-unit.md).

A Mitsubishi head compares its setpoint with its own view of the room (the
remote temperature, rounded to 0.5°C). At its view or above it starts within
seconds; 0.5°C below it keeps running, and a stopped unit restarts on its own
once its anti-short-cycle timer allows; 1.0°C below it stays off. Rounding a
continuous PID output onto that grid turns small corrections into full starts.

With `start_control: true` the PID only supplies demand, and a state machine
sends one of three setpoints relative to the unit's view:

| State | Setpoint | Entered when |
|---|---|---|
| IDLE | view − 1.0 | after every stop; the unit stays off |
| START | view | the PID's demand reaches `start_threshold`, no heat is arriving from other heads, and the room isn't still rising |
| RUN | view at start − 0.5 | the unit reports running; it stops by itself once its view moves one step, or is stopped early when the predicted peak reaches the target |
| PARKED | device off | the room is `off_threshold` past the target: at once while heat arrives from other heads, otherwise after the hysteresis `off_delay` (or at once past `off_limit`) |

A powered head on a shared outdoor unit keeps receiving refrigerant while
another head runs, and its fan carries that heat into the room. Only switching
it off stops that, hence PARKED. Power follows the hysteresis band:

- Inside the band the head stays powered, taking heat from other heads while
  they run instead of switching off and on for every small rise.
- Past `off_threshold` it is parked. Heat from other heads won't stop while it
  is powered, so with shared heat it is parked at once rather than after
  `off_delay`. Without shared heat `off_delay` and `off_limit` apply as usual,
  and a run is stopped while the hysteresis holds.
- A parked head is switched back on, idle, once the room is `on_threshold`
  short of the target. Hysteresis itself never switches the head on.

The band width trades comfort for power cycles: a wider band means longer
stretches powered and longer stretches parked.

- `start_threshold` (default 0.5°C) must be below `max_adjustment_over`. Once a
  few runs are learned it follows the room: half the learned gain of a start.
- Start control requires `hysteresis`.
- Heat arriving from other heads is detected on the head itself (the room
  rising while the head is idle and past its own coast, with humidity falling
  when `humidity_sensor` is set). A supervisor can also tell the head directly
  with the `set_shared_heat_hint(active, valid_for_s)` method, exposed as an
  ESPHome action; the hint expires on the head.
- What start control learns is saved across reboots like the room model;
  `reset_tuning()` clears it too.

```yaml
api:
  actions:
    - action: shared_heat
      variables: {active: bool, valid_for_s: int}
      then:
        - lambda: id(hvac).set_shared_heat_hint(active, valid_for_s);
```

The [`climate_adaptivepid_supervisor`](custom_components/climate_adaptivepid_supervisor)
Home Assistant integration (installable through HACS) sends that hint from
the other heads' `Operating` sensors. See
[docs/supervisor-design.md](docs/supervisor-design.md).

### Continuous tuning

With `adaptive: true` (the default) `kp` and `ki` are retuned while the
controller runs, and the configured values are only the starting point. The
controller learns a model of the room (`room_model.h`):

```
heating:  dT/dt =  heating_rate * u  -  heat_loss * (T_room - T_outdoor)
cooling:  dT/dt = -cooling_rate * u  -  heat_loss * (T_room - T_outdoor)
```

- `heat_loss`: how fast the room drifts towards the outdoor temperature.
- `heating_rate` / `cooling_rate`: how fast the device moves the room at full
  output.
- `u`: the device's output, 0 to 1. Units that report no output (no
  compressor frequency) get it estimated from the setpoint they were given:
  full output once the setpoint is `full_output_gap` past the room
  temperature, proportionally less below that.

It is learned in 15 minute windows, so a sensor's 0.1°C steps average out.
Idle windows (device not operating, room at least 3°C from outdoors) measure
the heat loss. Operating windows at a mean output of at least 0.3 measure the
change, take out the drift the heat loss predicts, and divide by the output:
one sample of the heating or cooling rate. Each sample moves the estimate 20%
of the way (heat loss 10%). A room held steady at low output therefore does not
read as a slow room.

After every start or stop the room keeps reacting to the previous state for a
while: the dead time, learned per mode from when the room turns after a start
(where it stops falling after a heating start, or rising after a cooling one).
No window is taken inside it. A defrost cycle, reported by the device as
`DEFROSTING`, delivers no heat: no window is taken during one, and the model
waits a dead time after it. A window in which the room moves the wrong way
faster than with the device off (an open window, say) is dropped.

The gains follow the SIMC tuning rule for an integrating process, per mode:

```
kp = full_output_gap / (rate * tc)      ki = kp / (4 * tc)      tc = response_time + dead_time
```

A room that responds quickly gets a lower gain, a slow one a higher gain, and a
longer dead time a gentler loop. `kd` is never changed. A configured `ki` is
used for a mode until that mode's first measurement; with `ki: 0` the tuned
`ki` is used from the start.

| `pid` option | Default | Meaning |
|---|---|---|
| `adaptive` | `true` | Retune `kp` and `ki` continuously. `false` keeps the configured gains. |
| `initial_heat_rate` / `initial_cool_rate` | from `kp` | Starting heating / cooling rate at full output, °C/h (0.2 to 10). Without them both start from the rate `kp` implies, else 2 °C/h. |
| `initial_heat_loss` | `0.1` | Starting heat loss, per hour (0 to 2). 0.1 is a drift of 1°C/h at 10°C indoor-outdoor difference. |
| `response_time` | `10min` | How quickly the loop should correct an error. Longer is gentler. |
| `dead_time` | `5min` | Starting dead time for both modes (learned between 1 and 30 minutes). |
| `full_output_gap` | `0.75` | How far the device setpoint must be past the room temperature for the device to run at full output. |
| `kp_min` / `kp_max` | `0.25` / `4.0` | Bounds on the tuned `kp`. |

The gains in use are published as `PID Kp` and `PID Ki`, and the model as
`Heating rate`, `Cooling rate` (°C/h), `Heat loss` (1/h), `Dead time heating`
and `Dead time cooling` (min).

#### Learned values across reboots and flashes

The model is saved to flash at most once an hour while it changes, and always
on a safe reboot, which includes every OTA update. Each value is stored with
the starting value (seed) it was learned from, and at boot each starts from:

- **what was learned**, when its seed is unchanged. Flashing new firmware with
  the same configuration keeps what the unit has learned.
- **the configured seed**, when the seed changed (`initial_heat_rate`,
  `initial_cool_rate`, `kp` without them, `initial_heat_loss`, `dead_time`) or
  nothing is stored. Editing a seed is taken as deliberate and applies on the
  next flash.

The boot log and `dump_config` say which applied to each value (`restored` or
`configured`). To make learned values the new starting point, copy them into
the matching options; the unit carries on from them. Response rates learned by
earlier versions (storage v1 and v2) are kept on upgrade as the heating and
cooling rates.

`reset_tuning()` forgets what was learned and starts from the seeds. Expose it
as an API action to call it from Home Assistant
(`esphome.<device>_reset_tuning`):

```yaml
api:
  actions:
    - action: reset_tuning
      then:
        - lambda: id(hvac_workflow).reset_tuning();
```

#### Replaying recorded history

`tools/room_model/replay.cpp` runs a Home Assistant history export through the
same model, to see what a unit would learn from it and to check the output
estimate: rates learned from full-output and from partial-output windows should
agree. Usage is at the top of the file. The export holds states only, so
defrost is not visible there and is caught only as wrong-way windows.

## Behaviour

- The workflows run in `HEAT` and `COOL`. In every other mode, and for fan,
  swing, presets and humidity, the outer climate passes straight through.
- In `HEAT`/`COOL` the outer target is the user's. It is stored per mode and is
  never overwritten by the device's setpoint.
- Two-point (low/high) inner climates are rejected at setup.

### Changes made at the device (e.g. the IR remote)

A generic climate cannot say who changed it, and it gives no acknowledgement
that a command arrived. The wrapper therefore ignores the inner climate's
reported power for `settle_time` after each of its own power commands, and
only treats a change as external once its own command has been seen to take
effect.

| Situation | Result |
|---|---|
| Device on and confirmed, then reports off | External off: the outer climate turns `OFF`. |
| Commanded on, device never came on | Lost command: sent again after `min_cycle_duration`. |
| Outer `OFF`, device comes on | The outer climate follows the device's mode. |
| Device changes between running modes | The outer climate follows, using that mode's stored target. |
| Workflow switched the device off, device is on anyway | Logged as an error and the internal state is resynced; the next workflow run decides what to do. |
| Setpoint changed at the device in `HEAT`/`COOL` | Overwritten by the controller. |

After a reboot, a restored `HEAT`/`COOL` with the device off is kept as is and
hysteresis (or, with start control, the start controller) decides when to
switch it on. Otherwise the outer climate starts from
the device's state.

## Development

Install `uv` and a C++17 compiler. From the repository root, `uv run --locked`
sets up the project environment with ESPHome 2026.10.2 and the dependencies in
`uv.lock`.

```bash
# Run all standalone C++ tests
uv run --locked python tests/run_native.py

# Host build against the demo climate
uv run --locked esphome compile test.yaml

# Run the end-to-end scenario on the host (about a minute; watch the log)
uv run --locked esphome compile tests/fixtures/scenario.yaml
tests/fixtures/.esphome/build/workflow-scenario/.pioenvs/workflow-scenario/program

# Start control on the host (a smoke test of the wiring)
uv run --locked esphome compile tests/fixtures/start_control.yaml
tests/fixtures/.esphome/build/start-control-scenario/.pioenvs/start-control-scenario/program

# Compile check against the upstream CN105 driver
uv run --locked esphome compile tests/fixtures/cn105.yaml

# Supervisor integration tests (an isolated environment with its own Home Assistant)
uv run --isolated --no-project --python 3.13 \
  --with pytest-homeassistant-custom-component \
  pytest tests/supervisor -o pythonpath=. -o asyncio_mode=auto
```

Layout of `components/climate_adaptivepid/`:

- `climate_adaptivepid.*`: the outer climate.
- `climate_device_adapter.*`: presents the inner climate to the controller.
- `power_tracker.h`: commanded versus reported power; no ESPHome dependency.
- `hysterisis_workflowstep.*`, `pid_workflowstep.*`, `adaptive_pid.*`: the controller.
- `room_model.h`: the learned room model and the output estimate; no ESPHome dependency.
- `tuning_rule.h`: the gains that follow from the model; no ESPHome dependency.
- `tuning_store.h`: whether each learned value starts from what was learned or its seed at boot; no ESPHome dependency.
- `controller_types.h`: the interface between the controller and the device.
- `start_controller.h`, `shared_heat_detector.h`, `burst_learner.h`: start control; no ESPHome dependency.
- `off_gate.h`: when hysteresis switches a running device off; no ESPHome dependency.

`custom_components/climate_adaptivepid_supervisor/`: the Home Assistant
integration that coordinates the heads of one outdoor unit; its decisions are
in `logic.py`, with no Home Assistant dependency.
