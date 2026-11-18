"""Cross-head decisions for climate_adaptivepid heads on one outdoor unit.

Pure Python with no Home Assistant imports, so it can be tested on its own
(tests/supervisor/test_logic.py). See docs/supervisor-design.md and
docs/start-control-design.md.

Heads on a shared outdoor unit leak heat (cooling) into each other: while any
head runs, an idle head that is still powered keeps receiving refrigerant and
its fan carries that into its room. A head can only infer that from its own
room; the supervisor sees every head's `operating` and knows for certain.

For each room that receives hints, shared heat is on while any *other* head in
the group is operating; the room's own run never counts. The head decides what
to do with it: it starts no run of its own while shared heat is on, and parks
(switches off) at once once its room is past its hysteresis off_threshold.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Head:
    """What the supervisor knows about one head."""

    name: str
    operating: bool | None = None
    # "heat", "cool" or anything else (no hints outside heat and cool).
    mode: str | None = None


@dataclass(frozen=True)
class RoomPlan:
    """What to tell one hinted room's head."""

    shared_heat: bool
    reason: str


def outdoor_unit_running(heads: dict[str, Head]) -> bool:
    """Whether any head in the group is operating."""
    return any(head.operating for head in heads.values())


def plan_room(name: str, heads: dict[str, Head]) -> RoomPlan | None:
    """The hint for one room, or None when nothing should be sent."""
    head = heads.get(name)
    if head is None or head.mode not in ("heat", "cool"):
        return None
    running = sorted(other.name for key, other in heads.items() if key != name and other.operating)
    if not running:
        return RoomPlan(shared_heat=False, reason="no other head running")
    return RoomPlan(shared_heat=True, reason=f"{', '.join(running)} running")


def plan(heads: dict[str, Head], hinted: list[str]) -> dict[str, RoomPlan]:
    """The hints for every hinted room that should get one."""
    plans: dict[str, RoomPlan] = {}
    for name in hinted:
        room_plan = plan_room(name, heads)
        if room_plan is not None:
            plans[name] = room_plan
    return plans
