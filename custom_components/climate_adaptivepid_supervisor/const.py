"""Constants for the climate_adaptivepid supervisor."""

from __future__ import annotations

DOMAIN = "climate_adaptivepid_supervisor"

CONF_HEADS = "heads"
CONF_HINTED = "hinted"

DEFAULT_NAME = "Outdoor unit"

# Hints expire on the head after this; they are renewed every RENEW_SECONDS
# while they still apply, so a stopped supervisor leaves the heads alone within
# a few minutes.
SHARED_HEAT_VALID_SECONDS = 120
RENEW_SECONDS = 60

# ESPHome actions defined in the heads' YAML (api: actions:).
ACTION_SHARED_HEAT = "shared_heat"
# Forecast: another head is expected to start in expected_in_s (-1 clears it).
# Optional on the head; a head without it gets shared_heat only.
ACTION_SHARED_HEAT_EXPECTED = "shared_heat_expected"

# The climate_adaptivepid diagnostic entity that reports the unit running.
OPERATING_NAME = "Operating"

# Learned starts and statistics are saved this long after a start, and at unload.
STORAGE_VERSION = 1
SAVE_DELAY_SECONDS = 60

PLATFORMS = ["binary_sensor", "sensor", "switch"]
