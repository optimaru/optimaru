"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`fcs.solve(schedule)`) enables all six constraints at the provider weights.
This module is the alternative path next to it: pick constraints, change weights, flip
hard/soft, and pass the list to `solve(schedule, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import flight_crew as fcs
    from flight_crew.constraints import (
        RequiredSkill, flight_conflict, get_constraints,
    )

    fcs.solve(schedule, constraints=get_constraints())          # defaults as-is
    fcs.solve(schedule, constraints=get_constraints([           # picked
        flight_conflict, RequiredSkill(weight=500),
    ]))

Each constraint has a default instance at module level, so you can also pass a
list of just a few. Only listed constraints are scored — leave one out and its
term disappears from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = component default.
- ``hard``     ``True``/``False`` flips the default level; ``None`` keeps it.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. 2.0 = squared, so concentrated violations grow much costlier.

Names (`name`) are the C++ component names in snake_case. The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`fcs.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "RequiredSkill",
    "FlightConflict",
    "TransferBetweenTwoFlights",
    "EmployeeUnavailability",
    "FirstAssignmentNotDepartingFromHome",
    "LastAssignmentNotArrivingAtHome",
    "required_skill",
    "flight_conflict",
    "transfer_between_two_flights",
    "employee_unavailability",
    "first_assignment_not_departing_from_home",
    "last_assignment_not_arriving_at_home",
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


class RequiredSkill(Constraint):
    """Crew assignments lacking the skill the seat requires. Default: hard, weight 100.

    The heaviest hard rule of the provider — a crew member who cannot take the seat is
    worse than any scheduling inconvenience."""

    name = "required_skill"


class FlightConflict(Constraint):
    """Pairs of the same crew member's assignments that overlap in time. Default: hard, weight 10.

    The plain sum over pairs **within** one itinerary, so the exact delta an assignment
    brings in or out is its overlap count with the other members."""

    name = "flight_conflict"


class TransferBetweenTwoFlights(Constraint):
    """Arrival airport differing from the next departure airport. Default: hard, weight 1.

    Someone has to move the crew in between by plane or car. "Next" follows the Java
    stream exactly — assignments with the same departure time do not start a pair, and
    all ties at the later end count."""

    name = "transfer_between_two_flights"


class EmployeeUnavailability(Constraint):
    """Assignments touching a date the crew member is unavailable. Default: hard, weight 10.

    Caught if any calendar date the flight spans, from departure date to arrival date
    inclusive, is unavailable."""

    name = "employee_unavailability"


class FirstAssignmentNotDepartingFromHome(Constraint):
    """The first assignment not departing from the home airport. Default: soft, weight 1000.

    It means getting there on one's own time. All ties at the earliest departure
    time count."""

    name = "first_assignment_not_departing_from_home"


class LastAssignmentNotArrivingAtHome(Constraint):
    """The last assignment not arriving at the home airport. Default: soft, weight 1000.

    The roster ends with the crew member stuck away from home. All ties at the latest
    departure time count."""

    name = "last_assignment_not_arriving_at_home"


#: Instances at the default weights. Reference them directly when picking.
required_skill = RequiredSkill()
flight_conflict = FlightConflict()
transfer_between_two_flights = TransferBetweenTwoFlights()
employee_unavailability = EmployeeUnavailability()
first_assignment_not_departing_from_home = FirstAssignmentNotDepartingFromHome()
last_assignment_not_arriving_at_home = LastAssignmentNotArrivingAtHome()

#: Default set — same order as `fcs.list_constraints()`.
_DEFAULTS: list[Constraint] = [
    required_skill,
    flight_conflict,
    transfer_between_two_flights,
    employee_unavailability,
    first_assignment_not_departing_from_home,
    last_assignment_not_arriving_at_home,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all six default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([flight_conflict, RequiredSkill(weight=500)])
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

    ``costs`` is what `fcs.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = fcs.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, picked)             # picked list

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
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
