"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`fp.solve(schedule)`) enables all five constraints at the default weights.
This module is the alternative path next to it: pick constraints, change weights, flip
hard/soft, and pass the list to `solve(schedule, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import food_packaging as fp
    from food_packaging.constraints import (
        MinimizeMakespan, get_constraints, max_end_date_time,
    )

    fp.solve(schedule, constraints=get_constraints())           # defaults as-is
    fp.solve(schedule, constraints=get_constraints([            # picked
        max_end_date_time, MinimizeMakespan(weight=10),
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
`fp.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "MaxEndDateTime",
    "IdealEndDateTime",
    "MinimizeMakespan",
    "OperatorCleaningConflict",
    "MaximizeJobsAssigned",
    "max_end_date_time",
    "ideal_end_date_time",
    "minimize_makespan",
    "operator_cleaning_conflict",
    "maximize_jobs_assigned",
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


class MaxEndDateTime(Constraint):
    """Minutes by which a job passes a deadline it must not miss. Default: hard, weight 1.

    Unassigned jobs are silent here — `maximize_jobs_assigned` values them."""

    name = "max_end_date_time"


class IdealEndDateTime(Constraint):
    """Minutes by which a job passes its ideal end date. Default: medium, weight 10^9.

    Missing the ideal date is a cost; missing `max_end_date_time` is a violation —
    that is why the shape is the same but the level differs. The medium level is folded
    into the engine soft level as `medium * kMediumUnit`."""

    name = "ideal_end_date_time"


class MinimizeMakespan(Constraint):
    """The **square** of each line's busy time. Default: soft, weight 1.

    Squaring pushes toward spreading jobs evenly across lines rather than just shrinking
    the total. An empty line costs nothing.

    The square lives inside cost() rather than in `exponent` on purpose — exponent
    is the knob that shapes the **weighted** cost, so keeping the square in the
    definition lets callers layer more shaping on top."""

    name = "minimize_makespan"


class OperatorCleaningConflict(Constraint):
    """Overlapping cleaning minutes on lines sharing one operator. Default: hard, weight 1.

    One person cannot clean two machines at once. The conflicting jobs sit on
    **different** lines, so this is a global constraint (two jobs on one line have
    strictly sequential cleaning windows and cannot overlap)."""

    name = "operator_cleaning_conflict"


class MaximizeJobsAssigned(Constraint):
    """Production minutes of jobs on no packaging line. Default: medium, weight 10^9.

    Dropping a long job hurts more than dropping a short one. A job can be in no list
    at all (the initial state before the construction heuristic), so the condition is
    "on no line", which is global rather than a property of any single unit."""

    name = "maximize_jobs_assigned"


#: Instances at the default weights. Reference them directly when picking.
max_end_date_time = MaxEndDateTime()
ideal_end_date_time = IdealEndDateTime()
minimize_makespan = MinimizeMakespan()
operator_cleaning_conflict = OperatorCleaningConflict()
maximize_jobs_assigned = MaximizeJobsAssigned()

#: Default set — same order as `fp.list_constraints()` (unit first, global after).
_DEFAULTS: list[Constraint] = [
    max_end_date_time,
    ideal_end_date_time,
    minimize_makespan,
    operator_cleaning_conflict,
    maximize_jobs_assigned,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all five default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([max_end_date_time, MinimizeMakespan(weight=10)])
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

    ``costs`` is what `fp.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = fp.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, picked)             # picked list

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft). This domain folds
    medium into engine soft as multiples of kMediumUnit (10^9), so to compare
    with the split levels of `solution.score`, fold the same way first.
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
