"""Download diagnostics: what the supervisor has learned and is sending, per head."""

from __future__ import annotations

from dataclasses import asdict
from typing import Any

from homeassistant.core import HomeAssistant
from homeassistant.util import dt as dt_util

from . import SupervisorConfigEntry


async def async_get_config_entry_diagnostics(hass: HomeAssistant, entry: SupervisorConfigEntry) -> dict[str, Any]:
    coordinator = entry.runtime_data
    now = dt_util.utcnow().timestamp()

    def ago(t: float | None) -> float | None:
        """Seconds before now (negative: in the future)."""
        return None if t is None else round(now - t, 1)

    heads: dict[str, Any] = {}
    for climate in coordinator.heads:
        entities = coordinator.entities.get(climate)
        predictor = coordinator.predictors.get(climate)
        hold = coordinator.holds.get(climate)
        stats = coordinator.stats.head(climate)
        room_plan = coordinator.plans.get(climate)
        tracker = coordinator.recoveries.get(climate)
        model = coordinator.models.get(climate)
        heads[climate] = {
            "entities": asdict(entities) if entities else None,
            "hinted": climate in coordinator.hinted,
            "plan": asdict(room_plan) if room_plan else None,
            "sent_shared_heat": coordinator._sent.get(climate),
            "sent_expected_in_s": coordinator._sent_expected.get(climate, "unsent"),
            "has_expected_action": climate not in coordinator._no_expected_action,
            "predictor": None
            if predictor is None
            else {
                "deficits": {mode: list(values) for mode, values in predictor.deficits.items()},
                "operating": predictor.operating,
                "mode": predictor.mode,
                "target": predictor.target,
                "last_stop_s_ago": ago(predictor.last_stop),
                "last_change_s_ago": ago(predictor.last_change),
                "samples": [[ago(t), room] for t, room in predictor.samples],
            },
            "operating_hold": None
            if hold is None
            else {"known": hold.known, "unknown_for_s": ago(hold.unknown_since)},
            "recovery": {
                "model": asdict(model) if model else None,
                "estimate_min": coordinator.recovery_estimates.get(climate),
                "timing": None
                if tracker is None or tracker.active is None
                else {"started_s_ago": ago(tracker.active[0]), "mode": tracker.active[1], "predicted_min": tracker.active[2]},
                "results": tracker.to_dict()["results"] if tracker else [],
            },
            "stats": {
                **stats.to_dict(),
                "hit_rate": stats.hit_rate,
                "last_error_s": stats.last_error_s,
                "last_start_s_ago": ago(stats.last_start),
                "last_start_stacked": stats.last_start_stacked,
                "forecast_start_in_s": None if stats.forecast_at is None else round(stats.forecast_at - now, 1),
            },
        }
    conflict = coordinator.mode_conflict
    return {
        "title": entry.title,
        "version": entry.version,
        "enabled": coordinator.enabled,
        "outdoor_unit_running": coordinator.outdoor_running,
        "outdoor_entity": coordinator.outdoor_entity,
        "outdoor": coordinator.outdoor,
        "mode_conflict": asdict(conflict) if conflict else None,
        "heads": heads,
    }
