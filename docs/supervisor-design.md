# HVAC supervisor design

**Status:**
- **Release 1, coordination: implemented.** It is the `climate_adaptivepid_supervisor` Home Assistant integration, with no app. See [Release 1: coordination](#release-1-coordination-implemented).
- **Everything after that: proposed, not scheduled.** That covers drift and recovery, the house model, proposals, and the emergency mode switch.

The supervisor coordinates the `climate_adaptivepid` heads of one house that share an outdoor unit. The first need was coordination: telling a head when heat is leaking in from the others, and when to park. That needs no model, so it lives in a custom integration that reacts to state changes. The later goals are letting rooms drift to a low-power state when not in use, recovering them in time, switching mode in an emergency, and proposing schedule changes from observed use. They extend the integration first; an app (below) is only worth adding if modelling outgrows it.

The measurements behind the coordination are in [research/shared-outdoor-unit.md](research/shared-outdoor-unit.md), and the head-side behaviour is in [start-control-design.md](start-control-design.md).

## Contents

- [Release 1: coordination (implemented)](#release-1-coordination-implemented)
- [Context](#context)
- [Goals](#goals)
- [Who owns what](#who-owns-what)
- [How the parts communicate](#how-the-parts-communicate)
- [Repo layout](#repo-layout)
- [Phase 4a: Read-only data pipeline](#phase-4a-read-only-data-pipeline)
- [Phase 4b: House model, recovery time, simulation](#phase-4b-house-model-recovery-time-simulation-read-only)
- [Phase 4c: Proposals from observed use](#phase-4c-proposals-from-observed-use-read-only-changes-nothing)
- [Phase 5a: Firmware for drift](#phase-5a-firmware-for-drift-nothing-active-yet)
- [Phase 5b: Drift control with early recovery](#phase-5b-drift-control-with-early-recovery-per-room-opt-in)
- [Phase 5c: Replace the setpoint automations](#phase-5c-replace-the-setpoint-automations)
- [Phase 5d: Emergency mode switch](#phase-5d-emergency-mode-switch-in-the-integration-independent-of-the-app)
- [Rollout](#rollout)
- [Verification](#verification)

## Release 1: coordination (implemented)

**What it is.** A custom integration, `custom_components/climate_adaptivepid_supervisor/` in this repo, installed through HACS (Custom repositories → this repo, type Integration). It is event-driven, async Python on Home Assistant's event loop, with no app, websocket, token, Docker image or heavy dependencies.

**Config flow.**
- **Pick the heads on one outdoor unit:** their adaptive PID climates, at least two.
- **Pick the rooms that receive hints:** they need `start_control` and the two ESPHome actions below.
- **Set the park margin.**

For each head, the integration finds its `Operating` binary sensor and its ESPHome node name (from the head's ESPHome config entry) through the device registry.

**Decisions** (`logic.py`, pure Python, tested without Home Assistant). For each hinted room:

| Other heads | Room vs target | Hints sent |
|---|---|---|
| None operating | — | `shared_heat` off; no park request (an earlier one expires) |
| Any operating | within `park_margin` of target, or past it | `shared_heat` on, `request_park` park |
| Any operating | further below target than that | `shared_heat` on, `request_park` unpark (take the leak instead of starting a run) |

A room's own run never counts as shared heat for that room. Rooms in modes other than heat or cool get no hints.

**Sending** (`coordinator.py`):
- **When:** on any change of the heads' climate or `Operating` states, and renewed every 60 s.
- **Expiry:** hints are valid for 120 s (`shared_heat`) and 180 s (`request_park`), so a stopped supervisor leaves the heads alone within 3 minutes.
- **Calls:** `esphome.<node>_shared_heat` and `esphome.<node>_request_park`.
- **Failures:** a failed call (head offline) is logged and skipped; the other heads are unaffected.
- **Logbook:** every change of hints is written to the logbook against the room's climate.

**Head side.** The device YAML defines the two actions, calling the climate's `set_shared_heat_hint(active, valid_for_s)` and `request_park(park, valid_for_s)`. Without hints, each head's local detector and thresholds decide (see [start-control-design.md](start-control-design.md#independence)).

**Entities:**
- `switch.<name>_supervisor_enabled` (restored across restarts): while off, nothing is sent.
- `binary_sensor.<name>_outdoor_unit_running`: diagnostic, any head operating.
- Per hinted room:
  - `binary_sensor.<name>_<room>_shared_heat`: other heads only.
  - `sensor.<name>_<room>_supervisor_action`: `none`, `shared_heat`, `park`, `unpark` or `disabled`, with the reason as an attribute.

**Tests** (`tests/supervisor/`, run with `pytest-homeassistant-custom-component` in an isolated environment, see the README):
- **The decision rules.**
- **The hint lifecycle:** sent when another head starts, renewed, cleared when it stops, never for the room's own run.
- **Park within the margin and unpark below it.**
- **The disable switch.**
- **A head offline.**
- **The config flow.**

**Next steps for the integration**, before any app:
- **Hold off overshoot:** delay another head's run with a small low-power offset (5a's `apply_offset`) while a hinted room is near the top of its band.
- **Group heating:** with planned recovery (5b), unpark a room during another head's run before it needs one of its own.
- **Emergency mode switch (5d).**

## Context

**The house.** Three Mitsubishi indoor heads on **one shared outdoor unit**, a multi-split:
- **Outdoor unit:** MXZ-5C42NAHZ2.
- **Main floor:** an SVZ-KP30NA ducted air handler.
- **Primary and second bedroom:** MLZ-KP09NA cassettes.

So:

- **Mode:** all heads must run in the same mode (heat or cool).
- **Capacity:** the heads share the outdoor unit's capacity.
- **Defrost and compressor starts** happen once, for all of them.
- **Heat leaks between heads.** While any head runs, a powered idle head keeps receiving refrigerant and its fan blows that into its room: +0.16 °C/min in the second bedroom, +0.05 in the primary. Only switching the head off stops it. This is what Release 1 coordinates.

Each head runs `climate_adaptivepid` (this repo) on an ESP32 over the `cn105`
driver.

**What the heads already do on their own** (Phases 0–2):

- **Learn a room model** on the device: heating and cooling rate at full
  output, heat loss to the outdoor temperature, and dead time per mode. It is
  kept across reboots and flashes (see the README).
- **Skip defrost.** It is reported by the `cn105` driver, and the heads also
  expose `Sub mode`, `Stage` and `Error code`.
- **Fall back to their own sensor** when a room temperature that arrives
  through Home Assistant goes stale, and report it as `Remote temperature in use`.

**Inputs that are not available:**

- **Compressor frequency:** not reported by these units.
- **Energy:** the main floor's kWh reads 0, and the bedroom kWh cannot be
  attributed per room. Energy is therefore measured as outdoor-unit runtime
  (any head operating) plus each head's estimated output.

**Today's comfort logic is a set of Home Assistant automations**, which are
effectively schedules:

- **Main floor:** 70 °F heat / 72 °F cool from 06:30, and 66.5 °F / 73.5 °F
  from 23:00 or when a "home off" scene is pressed on the dining-room panel.
- **Primary bedroom:** heat 67 °F at sunrise and 68 °F at sunset; cool
  73.5 °F at sunrise and 68.5 °F at sunset.
- **Second bedroom:** a morning wake-up change at 07:30.
- **Mode:** an automation switches all three heads to heat or cool based on
  one room's temperature and the outdoor temperature.

Temperatures are rarely changed by hand, and the climates are not switched on
and off. Signs of use are indirect: lights, TVs, a Hatch Rest sound machine in
the second bedroom, the room panels, scenes such as "home off", and the
occasional manual adjustment.

## Goals

These goals are for the later, unscheduled phases. As originally proposed, the
supervisor was an HA app (all computation) with a custom integration as its
only connection to Home Assistant; Release 1 showed the integration alone is
enough for coordination, so the app is now optional. Each room gets one target per mode, and
**comfort windows** set by the user: the times the room may be in use.

- **Inside a window**, the room is held at target.
- **Outside it**, the room may drift to a low-power state within an allowed
  drift: colder while heating, warmer while cooling, never the other way.
- **It learns how long each room takes to recover**, from a house model that
  includes the shared outdoor unit, and starts recovery early, so the room is
  at target when the window opens. With the automations, recovery starts only
  when they fire.
- **It proposes, and never changes:** it compares the schedules with observed
  use (activity signals and manual adjustments) and proposes edits to the
  schedules, drifts or targets as Home Assistant repair issues. The user
  decides and makes the edit.
- **Emergency mode switch:** if any room goes far out of bounds in the wrong
  direction, all three heads switch mode together. This replaces the existing
  mode automation.

**Constraints**

- **The heads stay autonomous.** Anything the supervisor sets expires on the
  head, which then returns to the user's target.
- **No head-to-app connection.** Traffic goes heads ⇄ Home Assistant ⇄
  integration ⇄ app. The app has no Home Assistant API token.
- **Each value has one owner** (next section). The supervisor writes only
  through Home Assistant services, and only to allowlisted targets.

**Decisions**

- **Location:** the app, integration and simulator live in this repo, next to
  the component they depend on. One tag releases all three.
- **Image:** built and pushed to the owner's Harbor registry, as one multi-arch
  image if possible.
- **Comfort windows:** schedule helpers set by the user are the explicit,
  authoritative definition of when each room is typically in use. Observed use
  only produces proposals; the planner follows the schedules as written.
- **Mode control:** emergency only, all heads together. It runs in the
  integration, so it works while the app is stopped.

## Who owns what

Each value has exactly one authoritative store, and only its owner changes it;
everything else reads it.

| Value | Authoritative store | Changed by | How it's changed | Read by |
|---|---|---|---|---|
| Target, per mode | The head: adaptive PID climate target (already stored per mode on the unit) | The user | HA climate card / `climate.set_temperature` | Head, integration → app |
| Mode | The head: adaptive PID climate mode | The user; the integration's emergency rule | `climate.set_hvac_mode` | Head, integration → app |
| Allowed drift heating / cooling | The head: two `number` entities, stored on the unit | The user | HA number entities | Head (clamps the offset), integration → app |
| Comfort windows | Home Assistant: one `schedule` helper per room | The user | Settings → Devices & services → Helpers → Schedule | Integration → app (next start and end, read ahead) |
| Supervisor control on/off, comfort-vs-savings slider | The integration's own entities (restored after restart) | The user | HA switch and number | Integration (gates commands), app |
| Emergency bounds and enable | The integration's options | The user | Integration options flow | Integration |
| Supervisor offset (with expiry) | The head | The app only | app → integration → `esphome.<device>_apply_offset` | Head; shown as `Supervisor offset` / `Effective target` |
| Learned room model | The head (persisted) | The head; the app via `apply_tuning` | `esphome.<device>_apply_tuning` | Head, integration → app |
| House model, plans, recommendations | The app (`/data`) | The app | — | Integration sensors, ingress UI |
| Proposals (schedule, drift, target, settings) | The app (`/data`) | The app proposes; only the user changes the values they concern | The user edits the schedule, number or YAML; dismissing the repair rejects it | HA repair issues, ingress UI |

The integration holds no control state. It keeps only its own settings
entities, the last command per room (for rate limits and the logbook), and
display sensors the app fills. If the app or integration restarts, there is
nothing to re-apply: offsets expire on the heads.

## How the parts communicate

```
head ⇄ ESPHome native API ⇄ HA core ⇄ climate_adaptivepid_supervisor integration ⇄ websocket (internal Supervisor network, token) ⇄ HVAC Supervisor app
```

- **App:** the websocket server; computes everything. It has no Home Assistant
  API token and never talks to the heads.
- **Integration:** the websocket client; the only part that reads Home
  Assistant state or calls Home Assistant services.
- **Heads:** see only Home Assistant, as today. A supervisor command reaches a
  head as an ESPHome API action call made by Home Assistant.

**Command path** (one offset):

1. **App → integration:**
   `{"type": "command", "id": 41, "room": "primary_bedroom", "action": "set_offset", "offset": -0.5, "valid_for_s": 1800}`.
2. **The integration checks it** (room enabled, value finite, at most one
   command per room every 10 minutes), then calls
   `esphome.primarybedroomhvac_apply_offset` with
   `{offset: -0.5, valid_for_s: 1800}`.
3. **Home Assistant's ESPHome integration sends the action** over the head's
   existing API connection. The head clamps the offset to the low-power
   direction and its allowed drift, applies it, and starts the expiry.
4. **The head publishes `Supervisor offset` and `Effective target`.** The
   integration sees the new state and replies
   `{"type": "ack", "id": 41, "applied": -0.5}` with the value the head
   actually applied, which may be clamped.

**Messages** (JSON, one per websocket frame; every message carries `"v": 1`):

| Direction | Type | Content |
|---|---|---|
| integration → app | `hello` | Integration version; the token |
| app → integration | `welcome` | App version. On a mismatch the integration raises a repair and stays read-only |
| integration → app | `config` | Rooms; each room's entity map; its schedule's windows for the next 7 days; supervisor switch; comfort slider; drift values. Re-sent on any change |
| integration → app | `states` | Batched state changes for allowlisted entities: `entity_id`, state, the attributes used, `last_changed`, and whether a person made the change |
| app → integration | `history_request` | Entity ids and a time range |
| integration → app | `history` | Recorder states for that range, in chunks |
| app → integration | `command` | `set_offset` or `apply_tuning`, as above |
| integration → app | `ack` | Command id and the applied value, or the rejection reason |
| app → integration | `status` | Per room: planned offset, recovery start, backtest error, recommendation summary. Shown as integration sensors |
| app → integration | `proposals` | The current proposals (id, room, kind, exact change, evidence). The integration raises or clears one repair issue each |
| integration → app | `proposal_dismissed` | A proposal id ignored in Repairs |

- **Token:** generated by the app in `/data` on first start and passed to the
  integration in the Supervisor discovery payload. The websocket rejects
  connections without it.
- **Units:** temperatures are converted to °C in `states` and `history`, from
  each entity's `unit_of_measurement`.

## Repo layout

Additions to this repo:

- **`repository.yaml` + `climate_adaptivepid_supervisor/`:** the Home Assistant app.
  - `config.yaml`:
    - `image: <registry>/hvac-supervisor`, as one multi-arch manifest for
      amd64 and aarch64 (fallback: one image per architecture,
      `hvac-supervisor-{arch}`). The Home Assistant host must be able to pull
      it: either a public registry project, or a registry login added under
      App store → ⋮ → Registries.
    - `ingress: true`, `discovery: [climate_adaptivepid_supervisor]`.
    - `hassio_api: true` (needed only to post discovery),
      `homeassistant_api: false`.
  - A Dockerfile and a Python package `supervisor_app/`: websocket server,
    SQLite store, model fit (scipy), planner, ingress UI.
- **`custom_components/climate_adaptivepid_supervisor/` + `hacs.json`:** the integration.
  - Config flow (Supervisor discovery, with a manual host/port fallback).
  - Websocket client and state forwarder.
  - The emergency rule.
  - Entities, repairs, diagnostics.
- **`sim/`:** pybind11 bindings built straight from `components/climate_adaptivepid/`
  at the same commit.
  - Reuses `room_model.h`, `tuning_rule.h`, `tuning_store.h`,
    `adaptive_pid.*`, `floats.h` and `controller_types.h`.
  - Stubs stand in for `esphome/core/hal.h` and `log.h`.
  - A fake `IDeviceStateManager` lets `PidWorkflowStep` and
    `HysterisisWorkflowStep` run unchanged.
- **`.github/workflows/`:**
  - **CI:** the native tests and ESPHome compiles, app pytest,
    `pytest-homeassistant-custom-component`, simulator tests, hassfest and
    HACS validation.
  - **Release:** on a tag, build multi-arch with buildx and push to the
    registry. Registry credentials go in repo secrets, and the registry must be
    reachable from GitHub's runners; otherwise use a self-hosted runner.
- **One version** is shared by `config.yaml`, `manifest.json` and a new
  `CLIMATE_ADAPTIVEPID_VERSION` constant in the component.
- **To check while implementing:**
  - The Home Assistant app store finds only `climate_adaptivepid_supervisor/config.yaml`, not
    the ESPHome YAML fixtures.
  - HACS accepts `custom_components/` next to ESPHome's `components/`.

## Phase 4a: Read-only data pipeline

- **Config flow:** pick the three adaptive PID climates (one outdoor-unit group).
  For each room, pick:
  - its comfort schedule helper,
  - its activity signals (suggested from the room's Home Assistant areas:
    lights, media players, the panel, buttons and scenes),
  - for bedrooms, optional bedtime and wake markers (for the second bedroom,
    the Hatch Rest).
- **Head entities:** found through the device registry: room temperature,
  outside temperature, set point, operating, internal power, sub mode, stage,
  error code, remote temperature in use, and the learned model sensors.
- **Context entities:** `sun.sun` and the weather entity (cloud cover).
- **Messages:** `config`, `states` and `history`. The app backfills about 10
  days from the recorder, then keeps its own 1-minute history in `/data`, which
  Home Assistant backups include.
- **Integration entities:** `binary_sensor.climate_adaptivepid_supervisor_connected`, and per
  room `sensor.<room>_supervisor_status`.
- **Ingress UI:** data coverage and a timeline per room.

## Phase 4b: House model, recovery time, simulation (read-only)

- **Nightly fit** over 28 days. Each room gets:
  - **Heat loss** to the outdoor temperature.
  - **Head output:** the head's rate × estimated output (`estimate_output`,
    via the bindings).
  - **Sun:** elevation × (1 − cloud cover).
  - **Coupling** to the other rooms, regularised.
  - **Shared capacity:** delivered = u × min(1, capacity / total demand of the
    running heads), fitted from periods with one, two and three heads running.
  - **Defrost** is excluded for all heads at once.
- **Recovery time:** per room, the time from the drifted temperature back to
  target at the current outdoor temperature, allowing for other rooms
  recovering at the same time. This drives early recovery.
- **Backtest** on the last 7 days: 1-hour and 3-hour prediction error, and
  predicted versus actual recovery after each of the automations' setpoint
  changes.
- **Settings search:** simulate the real controller (`sim/`) and search per room
  over `response_time`, `full_output_gap`, hysteresis thresholds,
  `min_cycle_duration` and `max_adjustment_*`.
  - Cost = time outside target + outdoor-unit starts + runtime.
  - Results are proposed (4c) as YAML snippets with the simulated
    before/after; the user applies them by editing YAML and flashing.
- **Version check:** `CLIMATE_ADAPTIVEPID_VERSION` is published as a diagnostic
  text sensor on each head. The integration raises a repair if it differs
  from the simulator's.

## Phase 4c: Proposals from observed use (read-only; changes nothing)

How the signals turn into proposals:

1. **Evidence of use, per room and 15-minute slot, each day:**
   - **Activity:** a room light on, a media player playing, the panel touched
     (`Current Page` changes, screen wakes), or a button or scene pressed in
     that slot.
   - **Sleep (bedrooms):** every slot between a bedtime marker (the Hatch
     turning on, the "home off" scene) and the next wake marker (the Hatch
     turning off, or the first activity in the house). Sleep makes no
     activity, so markers stand in for it.
   - **Manual adjustments:** target changes made by a person. Home
     Assistant's state context carries a `user_id` for those; automations and
     the integration's own calls do not.
     - **Comfort direction** (raised while heating, lowered while cooling):
       the room is in use and not comfortable. It weighs 3 times an activity.
     - **Low-power direction:** less heating or cooling is wanted.
   - **No evidence is not proof of an empty room:** someone reading quietly
     leaves no trace, so absence is only ever used cautiously (step 3).
2. **Typical week:** per room and slot, weekdays and weekends kept apart, the
   share of days with evidence over the last 6 weeks. Recent weeks count more
   (3-week half-life). No proposals until a room has 3 weeks of data.
3. **Comparing with the schedule:**
   - **Use outside a window:** 30 minutes or more in a row with evidence on at
     least 60% of days, or comfort adjustments on 2 or more days.
     - Next to a window edge: propose moving the edge.
     - Elsewhere: propose a new block.
   - **Unused window time:** 60 minutes or more at a window edge with evidence
     on 10% of days or fewer, and no adjustments. Propose trimming the edge,
     marked as lower confidence. It never proposes cutting into the middle of
     a window.
   - **Times** are rounded to 15 minutes, and each proposal reports the median
     observed time.
4. **Beyond the schedule:**
   - **Comfort adjustments within an hour after a window opens**, on 2 or more
     days:
     - If the room was not yet at target, the supervisor's own recovery model
       was wrong; it corrects itself from measured recoveries, with no
       proposal.
     - If the room was at target and was still adjusted: propose a smaller
       drift, or opening the window earlier.
   - **Repeated low-power adjustments inside a window:** propose a lower
     target. For example: "lowered at about 22:00 on 3 nights: lower the
     target, or end the window at 22:00?".
   - **Controller settings** from 4b's settings search are proposed the same
     way.
5. **Delivery:**
   - **Weekly** (Sunday night), at most 3 proposals per room.
   - **One Home Assistant repair issue each** (Settings → Repairs), with:
     - the exact change, for example "Main floor, weekdays: window start
       06:30 → 06:00",
     - the evidence, for example "activity before 06:30 on 15 of 20 weekdays,
       median first activity 06:05",
     - a link to the helper or entity.
   - **The user makes the edit;** the issue clears itself once the schedule
     matches.
   - **Dismissing (Ignore)** is reported back. That proposal returns only if
     its evidence doubles, and not within 8 weeks.
   - **Ingress UI:** a heatmap per room (week × 15 minutes) of observed use
     with the schedule outlined, and the open and dismissed proposals.

Proposals change nothing, so they run from the read-only period onward and
help set up the schedules before drift control starts.

## Phase 5a: Firmware for drift (nothing active yet)

**Component** (`components/climate_adaptivepid/`):

- **Allowed drift:** `Allowed drift heating` and `Allowed drift cooling`
  `number` entities (0–2 °C, step 0.25, default 0), configured in a new
  `supervisor:` block in `climate.py`.
  - Their unit is °C, with no temperature device class, because Home
    Assistant would convert a temperature difference as if it were a
    temperature.
- **`apply_offset(offset, valid_for_s)`:**
  - Accepted only in the low-power direction: [−drift, 0] while heating,
    [0, +drift] while cooling.
  - It applies to the target the workflows use, not to the climate's target.
  - It eases back to 0 over 10 minutes after expiry, and clears on a target or
    mode change.
  - The PID target shifts without resetting the integral. That needs a new
    `AdaptivePID` method alongside `set_target`, which resets it.
- **`apply_tuning(version, heat_rate, cool_rate, heat_loss, heat_dead_min, cool_dead_min)`:**
  checks every value against the `RoomModel` bounds, applies all or none as
  learned values, and publishes `Applied tuning version`.
- **New sensors:** `Effective target`, `Supervisor offset`, and the version text
  sensor.
- **Tests:** native tests for direction and clamping, expiry, easing and
  clearing, and applying tuning all or none.

**Home Assistant config repo:** in `esphome/packages/base-hvac.yaml`, add the
`supervisor:` block, `api: actions:` entries for `apply_offset` and
`apply_tuning`, and the ref bump.

## Phase 5b: Drift control with early recovery (per room, opt-in)

- **Planner:** every 15 minutes, for the enabled rooms together, over the next
  12 hours.
  - **Inside a window:** offset 0.
  - **Outside it:** drift as far as the allowed drift and the slider permit.
  - **Before the next window:** start recovery at window start minus the
    predicted recovery time, plus the slider's margin. The recovery model
    corrects itself from each measured recovery.
  - **Staggering:** when two rooms would recover at once on the shared outdoor
    unit, the later window's room starts earlier.
- **Commands:** `set_offset`, valid for 30 minutes, re-sent every 15 minutes.
  If the app dies, offsets expire within half an hour.
- **Integration entities per room:**
  - `switch.<room>_supervisor_control` (default off),
  - `number.<room>_comfort_weight`: from savings to comfort; sets how far it
    drifts and how much margin it adds before a window,
  - `sensor.<room>_planned_offset`, with a reason attribute (for example
    "outside window until 06:30; recovery from 05:40, before the bedroom").
- **Logbook:** every command is recorded.

## Phase 5c: Replace the setpoint automations

Each automation becomes a target, a schedule and a drift: set them, enable
control, then delete the automation. Drift values are in °C on the heads.

| Room | Target (heat / cool) | Comfort window (schedule) | Allowed drift heating / cooling |
|---|---|---|---|
| Main floor | 70 / 72 °F | 06:30–23:00 daily | 3.5 °F ≈ 1.95 °C / 1.5 °F ≈ 0.85 °C |
| Primary bedroom | 68 / 68.5 °F | Evenings to mornings, e.g. 19:00–07:00 (fixed times replace sunset/sunrise) | 1 °F ≈ 0.55 °C / 5 °F ≈ 2.8 °C |
| Second bedroom | From its wake-up automation | Likewise | Likewise |

- **Primary bedroom cooling:** its 5 °F drift is beyond the 2 °C cap. Either
  raise the cap to 3 °C, or accept a smaller setback.
- **Early end:** the "home off" scene ends the main floor's day early today.
  The integration's options can name one such entity per room that closes the
  window early when it turns on.

## Phase 5d: Emergency mode switch (in the integration, independent of the app)

- **Rule:** the house is in heat and any room has been at or above the "too
  warm" bound for 15 minutes, with the outdoor temperature above its threshold.
  Then all three heads switch to cool together. The rule mirrors for cooling.
- **Defaults** come from the existing mode automation: rooms 64.5 °F and
  75 °F, outdoor 50 °F and 70 °F. They can be changed in the integration's
  options.
- **Limits:**
  - at most one switch every 6 hours,
  - a persistent notification and a logbook entry with the reason,
  - `switch.climate_adaptivepid_supervisor_emergency_mode` to disable it.
- **Writes:** `climate.set_hvac_mode` on the three adaptive PID climates. The
  existing mode automation is then deleted.

## Rollout

1. **Prerequisite:** Phases 0–2 running on all heads (done).
2. **Development loop:**
   - Run tests locally, as CI does.
   - **App on the host:** clone this repo into `/addons/climate_adaptivepid_supervisor` from
     the Terminal app. It appears under "Local apps" and builds on the host.
   - **Integration on the host:** copy `custom_components/climate_adaptivepid_supervisor`
     into `/config/custom_components/`, then restart Home Assistant.
3. **First release (4a):** tag the repo; CI pushes the image.
   - **App:** App store → ⋮ → Repositories → add this repo's URL (and a
     registry login under Registries if the image is private). Install
     **HVAC Supervisor** and start it.
   - **Integration:** HACS → Custom repositories → the same URL, type
     Integration. Install it and restart Home Assistant.
   - **Setup:** confirm the discovered app, pick the three climates, and create
     and pick each room's schedule. Schedules have no effect until 5b.
4. **Read-only (4a–4c), about 2 weeks:**
   - Watch the backtest error, and predicted versus actual recovery after the
     automations' morning changes.
   - Go on when 1-hour error is under 0.3 °C and the recovery prediction is
     within 15 minutes.
   - Review the weekly proposals (Settings → Repairs) and apply the ones worth
     taking.
5. **Emergency switch (5d):** enable it once 4a is stable, and delete the
   existing mode automation. It doesn't depend on the model.
6. **Firmware (5a):** release, bump the ref, add the actions and the
   `supervisor:` block, and flash. Drift defaults to 0, so nothing changes.
7. **One room at a time (5b–5c):**
   - Start with a bedroom: set its target, schedule and drift from the table.
   - Turn on supervisor control, disable that room's automation, and delete it
     after a good week.
   - Compare runtime, outdoor-unit starts and time at target during the window
     with the previous weeks. Then the other rooms, one at a time.
8. **Updates:** one tag updates the app (app store) and the integration (HACS);
   bump the ref for new firmware. A version mismatch keeps the integration
   read-only and raises a repair.

## Verification

- **4a:**
  - Integration tests: discovery and the token, allowlist, unit conversion,
    schedule windows in `config`, and rejection of commands while read-only.
  - On the host: continuous data after backfill, and stopping the app has no
    effect on the heads.
- **4b:**
  - Simulator tests: the bindings reproduce `tests/test_room_model.cpp`, and
    the simulated controller matches the host `tests/fixtures/scenario.yaml`
    run.
  - Shared-capacity fit on synthetic multi-head data.
  - Live: backtest error, and recovery prediction against the morning setpoint
    changes.
- **4c:**
  - Tests on synthetic weeks:
    - regular use produces no proposals,
    - early morning use produces a window-start proposal,
    - weekday and weekend use are kept apart,
    - sleep markers cover the night,
    - a one-off adjustment proposes nothing,
    - trimming proposals need evidence of no use and are marked lower
      confidence,
    - a dismissed proposal stays away for 8 weeks unless its evidence doubles.
  - Only person-made adjustments count, not automation or integration calls.
  - Live: the first weekly proposals are checked against what is known of the
    household.
- **5a:** native component tests, and on a head, from Home Assistant's
  developer tools:
  - `apply_offset` moves `Effective target` but not the climate target,
  - the wrong direction is rejected, and the offset stays within the drift,
  - it eases back after expiry and clears on a target or mode change.
- **5b:**
  - Planner tests: offset 0 inside windows, low-power only outside, at target
    when a window opens on the fitted model, staggering under shared capacity,
    and expiry within 30 minutes when the app stops.
  - Live: one week in one room.
- **5d:**
  - Rule tests: any room triggers, all three switch together, outdoor gating,
    one switch per 6 hours, the disable switch works, and it works with the
    app stopped.
