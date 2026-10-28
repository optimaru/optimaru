"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`ts.solve(schedule)`) enables all four constraints at the quickstart weights.
This module is the alternative path next to it: pick constraints, change weights, flip
hard/soft, and pass the list to `solve(schedule, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import tournament_scheduling as ts
    from tournament_scheduling.constraints import (
        EvenlyConfrontationCount, get_constraints, one_assignment_per_date_per_team,
    )

    ts.solve(schedule, constraints=get_constraints())           # defaults as-is
    ts.solve(schedule, constraints=get_constraints([            # picked
        one_assignment_per_date_per_team, EvenlyConfrontationCount(weight=10),
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

Names (`name`) are the C++ component names in snake_case. The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`ts.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "OneAssignmentPerDatePerTeam",
    "UnavailabilityPenalty",
    "FairAssignmentCountPerTeam",
    "EvenlyConfrontationCount",
    "one_assignment_per_date_per_team",
    "unavailability_penalty",
    "fair_assignment_count_per_team",
    "evenly_confrontation_count",
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


class OneAssignmentPerDatePerTeam(Constraint):
    """Pairs of assignments one team has on the same day (C(c, 2) per team-date cell). Default: hard, weight 1."""

    name = "one_assignment_per_date_per_team"


class UnavailabilityPenalty(Constraint):
    """(team, date) unavailability facts with at least one assignment on them. Default: hard, weight 1.

    Counted **per fact**, not per assignment — two facts in one cell give 2, three
    assignments on one fact still give 1 (Java's ifExists semantics).

    The C++ struct is named `UnavailabilityPenaltyConstraint`, but the component carries
    the name `unavailabilityPenalty` — the suffix only exists because a domain fact in the
    same namespace already uses that name, so it is not part of the name."""

    name = "unavailability_penalty"


class FairAssignmentCountPerTeam(Constraint):
    """LoadBalance unfairness of assignment counts per team. Default: medium, weight MEDIUM_SCALE.

    Only teams that actually have assignments enter the balance (the Java rule has no
    complement). The statistic crosses units, so this is a global constraint."""

    name = "fair_assignment_count_per_team"


class EvenlyConfrontationCount(Constraint):
    """LoadBalance unfairness of head-to-head counts per team pair. Default: soft, weight 1.

    Each pair with assignments on the same day counts as one head-to-head (a team is not
    paired with itself), and only pairs with at least one head-to-head enter the balance.
    The statistic is over team **pairs**, so it is a global constraint no unit owns."""

    name = "evenly_confrontation_count"


#: Instances at the default weights. Reference them directly when picking.
one_assignment_per_date_per_team = OneAssignmentPerDatePerTeam()
unavailability_penalty = UnavailabilityPenalty()
fair_assignment_count_per_team = FairAssignmentCountPerTeam()
evenly_confrontation_count = EvenlyConfrontationCount()

#: Default set — same order as `ts.list_constraints()` (unit first, global after).
_DEFAULTS: list[Constraint] = [
    one_assignment_per_date_per_team,
    unavailability_penalty,
    fair_assignment_count_per_team,
    evenly_confrontation_count,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all four default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([one_assignment_per_date_per_team, EvenlyConfrontationCount(weight=10)])
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

    ``costs`` is what `ts.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = ts.constraint_costs(schedule)
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
