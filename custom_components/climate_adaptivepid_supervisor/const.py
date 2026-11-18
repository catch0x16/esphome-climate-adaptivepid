"""Constants for the climate_adaptivepid supervisor."""

from __future__ import annotations

DOMAIN = "climate_adaptivepid_supervisor"

CONF_HEADS = "heads"
CONF_HINTED = "hinted"
CONF_PARK_MARGIN = "park_margin"

DEFAULT_NAME = "Outdoor unit"
DEFAULT_PARK_MARGIN = 0.2

# Hints expire on the head after these; they are renewed every RENEW_SECONDS
# while they still apply, so a stopped supervisor leaves the heads alone within
# a few minutes.
SHARED_HEAT_VALID_SECONDS = 120
PARK_VALID_SECONDS = 180
RENEW_SECONDS = 60

# ESPHome actions defined in the heads' YAML (api: actions:).
ACTION_SHARED_HEAT = "shared_heat"
ACTION_REQUEST_PARK = "request_park"

# The climate_adaptivepid diagnostic entity that reports the unit running.
OPERATING_NAME = "Operating"

PLATFORMS = ["binary_sensor", "sensor", "switch"]
