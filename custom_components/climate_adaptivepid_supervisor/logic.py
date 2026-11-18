"""Cross-head decisions for climate_adaptivepid heads on one outdoor unit.

Pure Python with no Home Assistant imports, so it can be tested on its own
(tests/supervisor/test_logic.py). See docs/supervisor-design.md and
docs/start-control-design.md.

Heads on a shared outdoor unit leak heat (cooling) into each other: while any
head runs, an idle head that is still powered keeps receiving refrigerant and
its fan carries that into its room. A head can only infer that from its own
room; the supervisor sees every head's `operating` and knows for certain.

For each room that receives hints:

- Shared heat: on while any *other* head in the group is operating. The room's
  own run never counts.
- Park: while other heads run and the room is within `park_margin` of its
  target (or past it), ask the head to park (switch off), so the leak can't
  overshoot.
- Unpark: while other heads run and the room is further below its target than
  that, ask the head to unpark, so it takes the leak instead of starting a run
  of its own.
- With no other head running there is no park request: the head decides alone.
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
    room: float | None = None
    target: float | None = None


@dataclass(frozen=True)
class RoomPlan:
    """What to tell one hinted room's head."""

    shared_heat: bool
    # True: park, False: unpark, None: no request (let any earlier one expire).
    park: bool | None
    reason: str


def outdoor_unit_running(heads: dict[str, Head]) -> bool:
    """Whether any head in the group is operating."""
    return any(head.operating for head in heads.values())


def plan_room(name: str, heads: dict[str, Head], park_margin: float) -> RoomPlan | None:
    """The hints for one room, or None when nothing should be sent."""
    head = heads.get(name)
    if head is None or head.mode not in ("heat", "cool"):
        return None
    running = sorted(other.name for key, other in heads.items() if key != name and other.operating)
    if not running:
        return RoomPlan(shared_heat=False, park=None, reason="no other head running")

    others = ", ".join(running)
    if head.room is None or head.target is None:
        return RoomPlan(shared_heat=True, park=None, reason=f"{others} running; room temperature unknown")
    sign = 1.0 if head.mode == "heat" else -1.0
    # How far the room is past its target, in the direction the leak pushes it.
    past = sign * (head.room - head.target)
    if past >= -park_margin:
        return RoomPlan(
            shared_heat=True,
            park=True,
            reason=f"{others} running; room {past:+.2f} from target, within {park_margin:.2f}: park",
        )
    return RoomPlan(
        shared_heat=True,
        park=False,
        reason=f"{others} running; room {past:+.2f} from target: take the leak",
    )


def plan(heads: dict[str, Head], hinted: list[str], park_margin: float) -> dict[str, RoomPlan]:
    """The hints for every hinted room that should get one."""
    plans: dict[str, RoomPlan] = {}
    for name in hinted:
        room_plan = plan_room(name, heads, park_margin)
        if room_plan is not None:
            plans[name] = room_plan
    return plans
