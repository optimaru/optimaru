"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`ms.solve(schedule)`) enables all twelve constraints at the provider weights.
This module is the alternative path next to it: pick constraints, change weights, flip
hard/soft, and pass the list to `solve(schedule, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import meeting_scheduling as ms
    from meeting_scheduling.constraints import (
        OverlappingMeetings, get_constraints, room_conflict,
    )

    ms.solve(schedule, constraints=get_constraints())           # defaults as-is
    ms.solve(schedule, constraints=get_constraints([            # picked
        room_conflict, OverlappingMeetings(weight=100),
    ]))

Each constraint has a default instance at module level, so you can also pass a
list of just a few. Only listed constraints are scored — leave one out and its
term disappears from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = component default.
- ``hard``     ``True``/``False`` flips the default level; ``None`` keeps it.
               **Medium constraints in this domain cannot be expressed with
               this knob** — ``hard=False`` folds them into soft and the medium
               meaning is lost. Leave medium terms at ``None``.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. 2.0 = squared, so concentrated violations grow much costlier.

Attendance conflicts split into three terms: hard when both are **required**, medium
when only one is required or both are preferred — levels separate impossible schedules
from merely regrettable ones.

Names (`name`) are the C++ component names in snake_case. The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`ms.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "AvoidOvertime",
    "RequiredRoomCapacity",
    "StartAndEndOnSameDay",
    "DoMeetingsAsSoonAsPossible",
    "AssignLargerRoomsFirst",
    "RoomConflict",
    "RequiredAttendanceConflict",
    "RequiredAndPreferredAttendanceConflict",
    "PreferredAttendanceConflict",
    "OneBreakBetweenConsecutiveMeetings",
    "OverlappingMeetings",
    "RoomStability",
    "avoid_overtime",
    "required_room_capacity",
    "start_and_end_on_same_day",
    "do_meetings_as_soon_as_possible",
    "assign_larger_rooms_first",
    "room_conflict",
    "required_attendance_conflict",
    "required_and_preferred_attendance_conflict",
    "preferred_attendance_conflict",
    "one_break_between_consecutive_meetings",
    "overlapping_meetings",
    "room_stability",
    "get_constraints",
]


@dataclass(frozen=True)
class Constraint:
    """Spec selecting one constraint: weight / level / shaping."""

    weight: float | None = None  # None = component default weight
    hard: bool | None = None  # None = component default level
    exponent: float = 1.0
    name: ClassVar[str] = ""

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} must set a registry `name`")


class AvoidOvertime(Constraint):
    """Meetings ending past the end of the calendar. Default: hard, weight 1.

    The case where no TimeGrain fact exists at the last grain index. The penalty is
    that index, so running further over costs more."""

    name = "avoid_overtime"


class RequiredRoomCapacity(Constraint):
    """Seats missing in the assigned room for the attendees. Default: hard, weight 1."""

    name = "required_room_capacity"


class StartAndEndOnSameDay(Constraint):
    """Meetings whose last grain falls on a different day than the first. Default: hard, weight 1.

    Counted per TimeGrain fact at the last index on another day (the shape of the
    Java join). Running off the calendar entirely gives 0 — `avoid_overtime`
    values that."""

    name = "start_and_end_on_same_day"


class RoomConflict(Constraint):
    """Grains in which two meetings use the same room at the same time. Default: hard, weight 1."""

    name = "room_conflict"


class RequiredAttendanceConflict(Constraint):
    """Overlapping grains of a person **required in both** meetings. Default: hard, weight 1.

    Someone who must be in two meetings at once makes the schedule impossible, so it
    is hard. A meeting listing the same person as required twice folds in here too."""

    name = "required_attendance_conflict"


class RequiredAndPreferredAttendanceConflict(Constraint):
    """Overlapping grains of a person required in one and preferred in the other. Default: medium, weight MEDIUM_SCALE.

    Both directions count — which is why an edge carries separate forward/backward
    weights and the component adds them."""

    name = "required_and_preferred_attendance_conflict"


class PreferredAttendanceConflict(Constraint):
    """Overlapping grains of a person **preferred in both**. Default: medium, weight MEDIUM_SCALE.

    Regrettable rather than impossible, so it sits at medium: below the hard rules
    and above every soft convenience term."""

    name = "preferred_attendance_conflict"


class DoMeetingsAsSoonAsPossible(Constraint):
    """Each meeting's own last grain index. Default: soft, weight 1.

    Pulls the whole schedule toward the front of the calendar."""

    name = "do_meetings_as_soon_as_possible"


class AssignLargerRoomsFirst(Constraint):
    """For every room strictly larger than the assigned one, the capacity difference. Default: soft, weight 1.

    Fills big rooms first and leaves small rooms for meetings that only fit there.
    Per-room totals are precomputed by `ScoreCalculator::prepare`."""

    name = "assign_larger_rooms_first"


class OneBreakBetweenConsecutiveMeetings(Constraint):
    """Two meetings back to back without a single grain between. Default: soft, weight 100.

    The most expensive soft term of the provider — a day packed with gapless meetings
    is what attendees actually complain about."""

    name = "one_break_between_consecutive_meetings"


class OverlappingMeetings(Constraint):
    """Grains in which any two meetings run at once. Default: soft, weight 10.

    Regardless of room or attendees. Spreads the calendar so meetings someone might
    want to attend both of do not overlap."""

    name = "overlapping_meetings"


class RoomStability(Constraint):
    """Shared attendees whose next meeting starts within two grains in a different room. Default: soft, weight 1.

    Unlike the three conflict rules above, this is not about overlap — it fires
    precisely when meetings do **not** overlap much (people have to pack up and walk)."""

    name = "room_stability"


#: Instances at the default weights. Reference them directly when picking.
avoid_overtime = AvoidOvertime()
required_room_capacity = RequiredRoomCapacity()
start_and_end_on_same_day = StartAndEndOnSameDay()
do_meetings_as_soon_as_possible = DoMeetingsAsSoonAsPossible()
assign_larger_rooms_first = AssignLargerRoomsFirst()
room_conflict = RoomConflict()
required_attendance_conflict = RequiredAttendanceConflict()
required_and_preferred_attendance_conflict = RequiredAndPreferredAttendanceConflict()
preferred_attendance_conflict = PreferredAttendanceConflict()
one_break_between_consecutive_meetings = OneBreakBetweenConsecutiveMeetings()
overlapping_meetings = OverlappingMeetings()
room_stability = RoomStability()

#: Default set — same order as `ms.list_constraints()` (unit first, global after).
_DEFAULTS: list[Constraint] = [
    avoid_overtime,
    required_room_capacity,
    start_and_end_on_same_day,
    do_meetings_as_soon_as_possible,
    assign_larger_rooms_first,
    room_conflict,
    required_attendance_conflict,
    required_and_preferred_attendance_conflict,
    preferred_attendance_conflict,
    one_break_between_consecutive_meetings,
    overlapping_meetings,
    room_stability,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all twelve default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([room_conflict, OverlappingMeetings(weight=100)])
    """
    if constraints is None:
        return list(_DEFAULTS)
    if not constraints:
        # Same contract as C++ applyConstraints: an empty list means "score
        # nothing", which is most likely a mistake. A filter that happens to come
        # out empty must not silently flip to "score everything".
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return list(constraints)


class Score(NamedTuple):
    """(hard, soft) returned by `score()`. Both are penalties, so <= 0."""

    hard: int
    soft: float


def _shape(cost: float, exponent: float) -> float:
    """Sign-preserving power. Rewards (negative costs) never flip into penalties."""
    if exponent == 1.0 or cost == 0.0:
        return cost
    return -((-cost) ** exponent) if cost < 0.0 else cost**exponent


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `ms.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = ms.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, picked)             # picked list

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft). This domain folds
    medium into engine soft as multiples of MEDIUM_SCALE, so to compare with the
    split levels of `solution.score`, fold the same way first.
    """
    hard = 0
    soft = 0.0
    for spec in get_constraints(constraints):
        entry = costs.get(spec.name)
        if entry is None:
            raise ValueError(
                f"unknown constraint {spec.name!r}; available: " + ", ".join(repr(name) for name in sorted(costs))
            )
        weight = entry["weight"] if spec.weight is None else spec.weight
        is_hard = entry["hard"] if spec.hard is None else spec.hard
        exponent = spec.exponent
        for cost in entry["costs"]:
            value = weight * _shape(cost, exponent)
            if is_hard:
                # Round per unit contribution like the engine (half-away-from-zero
                # like llround; Python's round is banker's rounding).
                hard += int(math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5))
            else:
                soft += value
    return Score(-hard, -soft if soft != 0.0 else 0.0)
