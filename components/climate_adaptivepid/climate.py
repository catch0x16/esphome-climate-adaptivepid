import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, climate, sensor
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_UPDATE_INTERVAL,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)

AUTO_LOAD = ["binary_sensor", "climate", "sensor"]

CONF_CLIMATE_ID = "climate_id"
CONF_TEMPERATURE_SENSOR = "temperature_sensor"
CONF_READY_SENSOR = "ready_sensor"
CONF_OUTDOOR_TEMPERATURE_SENSOR = "outdoor_temperature_sensor"
CONF_HUMIDITY_SENSOR = "humidity_sensor"
CONF_SETTLE_TIME = "settle_time"

CONF_PID = "pid"
CONF_KP = "kp"
CONF_KI = "ki"
CONF_KD = "kd"
CONF_MAX_ADJUSTMENT_UNDER = "max_adjustment_under"
CONF_MAX_ADJUSTMENT_OVER = "max_adjustment_over"
CONF_SETPOINT_DEADBAND = "setpoint_deadband"
CONF_START_CONTROL = "start_control"
CONF_START_THRESHOLD = "start_threshold"
CONF_PARK_WHEN_SHARED = "park_when_shared"
CONF_PARK_MARGIN = "park_margin"

CONF_ADAPTIVE = "adaptive"
CONF_RESPONSE_TIME = "response_time"
CONF_DEAD_TIME = "dead_time"
CONF_FULL_OUTPUT_GAP = "full_output_gap"
CONF_KP_MIN = "kp_min"
CONF_KP_MAX = "kp_max"
CONF_INITIAL_HEAT_RATE = "initial_heat_rate"
CONF_INITIAL_COOL_RATE = "initial_cool_rate"
CONF_INITIAL_HEAT_LOSS = "initial_heat_loss"

CONF_HYSTERESIS = "hysteresis"
CONF_ON_THRESHOLD = "on_threshold"
CONF_OFF_THRESHOLD = "off_threshold"
CONF_OFF_DELAY = "off_delay"
CONF_OFF_LIMIT = "off_limit"
CONF_MIN_CYCLE_DURATION = "min_cycle_duration"

CONF_INTERNAL_POWER = "internal_power"
CONF_DEVICE_POWER = "device_power"
CONF_OPERATING = "operating"
CONF_DEVICE_CURRENT_TEMPERATURE = "device_current_temperature"
CONF_DEVICE_SET_POINT = "device_set_point"
CONF_PID_SET_POINT = "pid_set_point"
CONF_PID_KP = "pid_kp"
CONF_PID_KI = "pid_ki"
CONF_HEATING_RATE = "heating_rate"
CONF_COOLING_RATE = "cooling_rate"
CONF_HEAT_LOSS = "heat_loss"
CONF_DEAD_TIME_HEATING = "dead_time_heating"
CONF_DEAD_TIME_COOLING = "dead_time_cooling"
CONF_CONTROL_STATE = "control_state"
CONF_START_THRESHOLD_SENSOR = "start_threshold_sensor"
CONF_LEAK_RATE = "leak_rate"
CONF_SHARED_HEAT = "shared_heat"

climate_adaptivepid_ns = cg.esphome_ns.namespace("climate_adaptivepid")
ClimateAdaptivePID = climate_adaptivepid_ns.class_(
    "ClimateAdaptivePID", climate.Climate, cg.Component
)

PID_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_KP, default=0.0): cv.float_,
        cv.Optional(CONF_KI, default=0.0): cv.float_,
        cv.Optional(CONF_KD, default=0.0): cv.float_,
        cv.Optional(CONF_MAX_ADJUSTMENT_UNDER, default=2.0): cv.positive_float,
        cv.Optional(CONF_MAX_ADJUSTMENT_OVER, default=2.0): cv.positive_float,
        # How far past a device step the correction must go before the device
        # setpoint steps towards more output; stops flipping at a step boundary.
        cv.Optional(CONF_SETPOINT_DEADBAND, default=0.1): cv.float_range(
            min=0.0, max=1.0
        ),
        # Start control (docs/start-control-design.md): the PID supplies demand,
        # and a state machine decides when the unit starts, sending one of three
        # setpoints relative to the unit's own view of the room. For units whose
        # thermostat starts at setpoint >= its view and stops at <= view - 1.0.
        cv.Optional(CONF_START_CONTROL, default=False): cv.boolean,
        # PID demand (output - target, C) that starts a run. Learned from the
        # room's response once there are enough runs.
        cv.Optional(CONF_START_THRESHOLD, default=0.5): cv.float_range(
            min=0.1, max=3.0
        ),
        # Parking follows the hysteresis band under start control: the head is
        # switched off past off_threshold (at once while heat arrives from other
        # heads) and back on at on_threshold.
        cv.Optional(CONF_PARK_WHEN_SHARED): cv.invalid(
            f"{CONF_PARK_WHEN_SHARED} was removed: under {CONF_START_CONTROL} the head "
            "parks past hysteresis off_threshold (at once while heat arrives from "
            "other heads) and switches back on at hysteresis on_threshold"
        ),
        cv.Optional(CONF_PARK_MARGIN): cv.invalid(
            f"{CONF_PARK_MARGIN} was removed: use hysteresis off_threshold, where "
            f"{CONF_START_CONTROL} parks the head while heat arrives from other heads"
        ),
        # Continuous tuning: kp and ki follow the measured response of the room.
        # The configured kp and ki are then only the starting values.
        cv.Optional(CONF_ADAPTIVE, default=True): cv.boolean,
        cv.Optional(
            CONF_RESPONSE_TIME, default="10min"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_DEAD_TIME, default="5min"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_FULL_OUTPUT_GAP, default=0.75): cv.float_range(
            min=0.1, max=5.0
        ),
        cv.Optional(CONF_KP_MIN, default=0.25): cv.float_range(min=0.01),
        cv.Optional(CONF_KP_MAX, default=4.0): cv.float_range(min=0.01),
        # Starting values for the learned room model. Rates at full output in
        # degrees per hour, per mode; without them both modes start from the
        # rate the configured kp implies. Heat loss per hour (0.1: a drift of
        # 1 C/h at 10 C indoor-outdoor difference). `dead_time` above is the
        # starting dead time. A unit keeps what it learned until a seed changes.
        cv.Optional(CONF_INITIAL_HEAT_RATE): cv.float_range(min=0.2, max=10.0),
        cv.Optional(CONF_INITIAL_COOL_RATE): cv.float_range(min=0.2, max=10.0),
        cv.Optional(CONF_INITIAL_HEAT_LOSS): cv.float_range(min=0.0, max=2.0),
    }
)


def _validate_start_control(conf):
    if conf[CONF_START_CONTROL] and conf[CONF_START_THRESHOLD] >= conf[CONF_MAX_ADJUSTMENT_OVER]:
        raise cv.Invalid(
            f"{CONF_START_THRESHOLD} must be below {CONF_MAX_ADJUSTMENT_OVER}: the PID's "
            "demand is capped there, so the unit could never start",
            path=[CONF_START_THRESHOLD],
        )
    return conf


PID_SCHEMA = cv.All(PID_SCHEMA, _validate_start_control)


def _gain_sensor_schema(decimals, unit=None):
    kwargs = {"unit_of_measurement": unit} if unit else {}
    return sensor.sensor_schema(
        accuracy_decimals=decimals,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        **kwargs,
    )

HYSTERESIS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ON_THRESHOLD, default=0.25): cv.positive_float,
        cv.Optional(CONF_OFF_THRESHOLD, default=0.25): cv.positive_float,
        # Past off_threshold the device is first held at least output (the PID
        # pulled down); it is switched off once the room has stayed past the
        # threshold this long, or at once past off_limit.
        cv.Optional(
            CONF_OFF_DELAY, default="0s"
        ): cv.positive_time_period_milliseconds,
        # Default: off_threshold + 0.5.
        cv.Optional(CONF_OFF_LIMIT): cv.positive_float,
        cv.Optional(
            CONF_MIN_CYCLE_DURATION, default="60s"
        ): cv.positive_time_period_milliseconds,
    }
)


def _validate_off_limit(conf):
    if CONF_OFF_LIMIT in conf and conf[CONF_OFF_LIMIT] <= conf[CONF_OFF_THRESHOLD]:
        raise cv.Invalid(
            f"{CONF_OFF_LIMIT} must be greater than {CONF_OFF_THRESHOLD}",
            path=[CONF_OFF_LIMIT],
        )
    return conf


HYSTERESIS_SCHEMA = cv.All(HYSTERESIS_SCHEMA, _validate_off_limit)


def _validate_start_control_hysteresis(config):
    # Start control parks and unparks the head on the hysteresis band.
    if (
        CONF_PID in config
        and config[CONF_PID][CONF_START_CONTROL]
        and CONF_HYSTERESIS not in config
    ):
        raise cv.Invalid(
            f"{CONF_START_CONTROL} requires {CONF_HYSTERESIS}: the head is parked past "
            "its off_threshold and switched back on at its on_threshold",
            path=[CONF_PID, CONF_START_CONTROL],
        )
    return config


def _temperature_sensor_schema():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


def _power_sensor_schema(device_class):
    return binary_sensor.binary_sensor_schema(
        device_class=device_class,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


# Diagnostic entities are created by default. The names match the entities the
# integrated mitsubishi_heatpump component used to publish.
CONFIG_SCHEMA = cv.All(
    climate.climate_schema(ClimateAdaptivePID)
    .extend(
        {
            cv.Required(CONF_CLIMATE_ID): cv.use_id(climate.Climate),
            cv.Optional(CONF_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_READY_SENSOR): cv.use_id(binary_sensor.BinarySensor),
            # Lets the room model learn heat loss and take it out of the
            # heating and cooling rates.
            cv.Optional(CONF_OUTDOOR_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
            # Room humidity, used with start control to confirm heat arriving
            # from other heads (warm supply air dries the room).
            cv.Optional(CONF_HUMIDITY_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(
                CONF_UPDATE_INTERVAL, default="2s"
            ): cv.positive_time_period_milliseconds,
            # Defaults to 2 x update_interval + 5s.
            cv.Optional(CONF_SETTLE_TIME): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_PID): PID_SCHEMA,
            cv.Optional(CONF_HYSTERESIS): HYSTERESIS_SCHEMA,
            cv.Optional(
                CONF_INTERNAL_POWER, default={CONF_NAME: "Internal power on"}
            ): _power_sensor_schema(DEVICE_CLASS_POWER),
            cv.Optional(
                CONF_DEVICE_POWER, default={CONF_NAME: "Device power on"}
            ): _power_sensor_schema(DEVICE_CLASS_POWER),
            cv.Optional(
                CONF_OPERATING, default={CONF_NAME: "Operating"}
            ): _power_sensor_schema(DEVICE_CLASS_RUNNING),
            cv.Optional(
                CONF_DEVICE_CURRENT_TEMPERATURE,
                default={CONF_NAME: "Current temperature"},
            ): _temperature_sensor_schema(),
            cv.Optional(
                CONF_DEVICE_SET_POINT, default={CONF_NAME: "Set Point"}
            ): _temperature_sensor_schema(),
            cv.Optional(
                CONF_PID_SET_POINT, default={CONF_NAME: "PID Set Point"}
            ): _temperature_sensor_schema(),
            cv.Optional(CONF_PID_KP, default={CONF_NAME: "PID Kp"}): _gain_sensor_schema(
                3
            ),
            cv.Optional(CONF_PID_KI, default={CONF_NAME: "PID Ki"}): _gain_sensor_schema(
                5
            ),
            cv.Optional(
                CONF_HEATING_RATE, default={CONF_NAME: "Heating rate"}
            ): _gain_sensor_schema(2, "°C/h"),
            cv.Optional(
                CONF_COOLING_RATE, default={CONF_NAME: "Cooling rate"}
            ): _gain_sensor_schema(2, "°C/h"),
            cv.Optional(
                CONF_HEAT_LOSS, default={CONF_NAME: "Heat loss"}
            ): _gain_sensor_schema(3, "1/h"),
            cv.Optional(
                CONF_DEAD_TIME_HEATING, default={CONF_NAME: "Dead time heating"}
            ): _gain_sensor_schema(1, "min"),
            cv.Optional(
                CONF_DEAD_TIME_COOLING, default={CONF_NAME: "Dead time cooling"}
            ): _gain_sensor_schema(1, "min"),
            # Created only with start control. Control state: 0 IDLE, 1 START,
            # 2 RUN, 3 PARKED.
            cv.Optional(
                CONF_CONTROL_STATE, default={CONF_NAME: "Control state"}
            ): _gain_sensor_schema(0),
            cv.Optional(
                CONF_START_THRESHOLD_SENSOR, default={CONF_NAME: "Start threshold"}
            ): _gain_sensor_schema(2, "°C"),
            cv.Optional(CONF_LEAK_RATE): cv.invalid(
                f"{CONF_LEAK_RATE} was removed along with learned parking"
            ),
            cv.Optional(
                CONF_SHARED_HEAT, default={CONF_NAME: "Shared heat"}
            ): _power_sensor_schema(DEVICE_CLASS_RUNNING),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.has_at_least_one_key(CONF_PID, CONF_HYSTERESIS),
    _validate_start_control_hysteresis,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    inner = await cg.get_variable(config[CONF_CLIMATE_ID])
    cg.add(var.set_inner_climate(inner))

    if CONF_TEMPERATURE_SENSOR in config:
        temperature = await cg.get_variable(config[CONF_TEMPERATURE_SENSOR])
        cg.add(var.set_temperature_sensor(temperature))
    if CONF_READY_SENSOR in config:
        ready = await cg.get_variable(config[CONF_READY_SENSOR])
        cg.add(var.set_ready_sensor(ready))
    if CONF_OUTDOOR_TEMPERATURE_SENSOR in config:
        outdoor = await cg.get_variable(config[CONF_OUTDOOR_TEMPERATURE_SENSOR])
        cg.add(var.set_outdoor_temperature_sensor(outdoor))
    if CONF_HUMIDITY_SENSOR in config:
        humidity = await cg.get_variable(config[CONF_HUMIDITY_SENSOR])
        cg.add(var.set_humidity_sensor(humidity))

    update_interval = config[CONF_UPDATE_INTERVAL]
    cg.add(var.set_update_interval(update_interval))
    if CONF_SETTLE_TIME in config:
        settle_ms = config[CONF_SETTLE_TIME].total_milliseconds
    else:
        settle_ms = 2 * update_interval.total_milliseconds + 5000
    cg.add(var.set_settle_time(settle_ms))

    if CONF_PID in config:
        conf = config[CONF_PID]
        cg.add(var.set_pid_enabled(True))
        cg.add(var.set_kp(conf[CONF_KP]))
        cg.add(var.set_ki(conf[CONF_KI]))
        cg.add(var.set_kd(conf[CONF_KD]))
        cg.add(var.set_max_adjustment_under(conf[CONF_MAX_ADJUSTMENT_UNDER]))
        cg.add(var.set_max_adjustment_over(conf[CONF_MAX_ADJUSTMENT_OVER]))
        cg.add(var.set_setpoint_deadband(conf[CONF_SETPOINT_DEADBAND]))
        cg.add(var.set_start_control(conf[CONF_START_CONTROL]))
        cg.add(var.set_start_threshold(conf[CONF_START_THRESHOLD]))
        cg.add(var.set_pid_adaptive(conf[CONF_ADAPTIVE]))
        cg.add(var.set_pid_response_time(conf[CONF_RESPONSE_TIME]))
        cg.add(var.set_pid_dead_time(conf[CONF_DEAD_TIME]))
        cg.add(var.set_pid_full_output_gap(conf[CONF_FULL_OUTPUT_GAP]))
        cg.add(var.set_pid_kp_min(conf[CONF_KP_MIN]))
        cg.add(var.set_pid_kp_max(conf[CONF_KP_MAX]))
        if CONF_INITIAL_HEAT_RATE in conf:
            cg.add(var.set_initial_heat_rate(conf[CONF_INITIAL_HEAT_RATE]))
        if CONF_INITIAL_COOL_RATE in conf:
            cg.add(var.set_initial_cool_rate(conf[CONF_INITIAL_COOL_RATE]))
        if CONF_INITIAL_HEAT_LOSS in conf:
            cg.add(var.set_initial_heat_loss(conf[CONF_INITIAL_HEAT_LOSS]))

    if CONF_HYSTERESIS in config:
        conf = config[CONF_HYSTERESIS]
        cg.add(var.set_hysteresis_enabled(True))
        cg.add(var.set_hysteresis_on(conf[CONF_ON_THRESHOLD]))
        cg.add(var.set_hysteresis_off(conf[CONF_OFF_THRESHOLD]))
        cg.add(var.set_hysteresis_off_delay(conf[CONF_OFF_DELAY]))
        cg.add(
            var.set_hysteresis_off_limit(
                conf.get(CONF_OFF_LIMIT, conf[CONF_OFF_THRESHOLD] + 0.5)
            )
        )
        cg.add(var.set_min_cycle_duration(conf[CONF_MIN_CYCLE_DURATION]))

    for key, setter in (
        (CONF_INTERNAL_POWER, var.set_internal_power_sensor),
        (CONF_DEVICE_POWER, var.set_device_power_sensor),
        (CONF_OPERATING, var.set_operating_sensor),
    ):
        cg.add(setter(await binary_sensor.new_binary_sensor(config[key])))

    for key, setter in (
        (CONF_DEVICE_CURRENT_TEMPERATURE, var.set_device_current_temperature_sensor),
        (CONF_DEVICE_SET_POINT, var.set_device_set_point_sensor),
    ):
        cg.add(setter(await sensor.new_sensor(config[key])))
    if CONF_PID in config:
        cg.add(
            var.set_pid_set_point_sensor(
                await sensor.new_sensor(config[CONF_PID_SET_POINT])
            )
        )
        cg.add(var.set_pid_kp_sensor(await sensor.new_sensor(config[CONF_PID_KP])))
        cg.add(var.set_pid_ki_sensor(await sensor.new_sensor(config[CONF_PID_KI])))
        if config[CONF_PID][CONF_ADAPTIVE]:
            for key, setter in (
                (CONF_HEATING_RATE, var.set_heating_rate_sensor),
                (CONF_COOLING_RATE, var.set_cooling_rate_sensor),
                (CONF_HEAT_LOSS, var.set_heat_loss_sensor),
                (CONF_DEAD_TIME_HEATING, var.set_dead_time_heating_sensor),
                (CONF_DEAD_TIME_COOLING, var.set_dead_time_cooling_sensor),
            ):
                cg.add(setter(await sensor.new_sensor(config[key])))
        if config[CONF_PID][CONF_START_CONTROL]:
            for key, setter in (
                (CONF_CONTROL_STATE, var.set_control_state_sensor),
                (CONF_START_THRESHOLD_SENSOR, var.set_start_threshold_sensor),
            ):
                cg.add(setter(await sensor.new_sensor(config[key])))
            cg.add(
                var.set_shared_heat_sensor(
                    await binary_sensor.new_binary_sensor(config[CONF_SHARED_HEAT])
                )
            )
