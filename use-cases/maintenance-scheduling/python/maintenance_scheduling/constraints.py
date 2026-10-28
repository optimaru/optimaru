"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`mnt.solve(schedule)`) enables all six constraints at the quickstart
weights. This module is the alternative path next to it: pick constraints, change weights,
flip hard/soft, and pass the list to `solve(schedule, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import maintenance_scheduling as mnt
    from maintenance_scheduling.constraints import (
        TagConflict, crew_conflict, get_constraints,
    )

    mnt.solve(schedule, constraints=get_constraints())          # defaults as-is
    mnt.solve(schedule, constraints=get_constraints([           # picked
        crew_conflict, TagConflict(weight=10_000),
    ]))

Each constraint has a default instance at module level, so you can also pass a
list of just a few. Only listed constraints are scored — leave one out and its
term disappears from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = component default.
- ``hard``     ``True``/``False`` flips the default level; ``None`` keeps it.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. 2.0 = squared, so concentrated violations grow much costlier.

The weight ratio of the three soft terms (1 : 1'000 : 1'000'000) is the character of this
domain — lateness is a thousand times a tag conflict and a million times early completion.

Names (`name`) are the C++ component names in snake_case. The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`mnt.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "MinStartDate",
    "MaxEndDate",
    "BeforeIdealEndDate",
    "AfterIdealEndDate",
    "CrewConflict",
    "TagConflict",
    "min_start_date",
    "max_end_date",
    "before_ideal_end_date",
    "after_ideal_end_date",
    "crew_conflict",
    "tag_conflict",
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


class MinStartDate(Constraint):
    """Calendar days a job starts before its earliest start date. Default: hard, weight 1."""

    name = "min_start_date"


class MaxEndDate(Constraint):
    """Calendar days a job ends after its end deadline. Default: hard, weight 1."""

    name = "max_end_date"


class BeforeIdealEndDate(Constraint):
    """Calendar days a job ends **before** its ideal end date. Default: soft, weight 1.

    The cheapest of the three soft terms by design — bringing maintenance forward
    throws away remaining service life, but that loss is light."""

    name = "before_ideal_end_date"


class AfterIdealEndDate(Constraint):
    """Calendar days a job ends **after** its ideal end date. Default: soft, weight 10^6.

    A thousand times a tag conflict and a million times early completion — which is why
    this is the soft term the search clears first."""

    name = "after_ideal_end_date"


class CrewConflict(Constraint):
    """Calendar days two jobs of the same crew overlap. Default: hard, weight 1.

    Each crew owns one row, so a job occupies exactly one row for the days it runs.
    Two jobs of the same crew can sit in different (start date, crew) cells, so this is
    a global constraint."""

    name = "crew_conflict"


class TagConflict(Constraint):
    """Calendar days two jobs sharing a tag overlap. Default: soft, weight 1000.

    Counted once **per shared tag**. Each tag owns one row, so a job occupies as many
    rows as it carries tags. Global for the same reason as `crew_conflict`."""

    name = "tag_conflict"


#: Instances at the default weights. Reference them directly when picking.
min_start_date = MinStartDate()
max_end_date = MaxEndDate()
before_ideal_end_date = BeforeIdealEndDate()
after_ideal_end_date = AfterIdealEndDate()
crew_conflict = CrewConflict()
tag_conflict = TagConflict()

#: Default set — same order as `mnt.list_constraints()` (unit first, global after).
_DEFAULTS: list[Constraint] = [
    min_start_date,
    max_end_date,
    before_ideal_end_date,
    after_ideal_end_date,
    crew_conflict,
    tag_conflict,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all six default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([crew_conflict, TagConflict(weight=10_000)])
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

    ``costs`` is what `mnt.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = mnt.constraint_costs(schedule)
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
