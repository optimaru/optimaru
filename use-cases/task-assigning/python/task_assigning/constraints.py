"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`ta.solve(solution)`) enables all six constraints at the quickstart
weights. This module is the alternative path next to it: pick constraints, change weights,
flip hard/soft, and pass the list to `solve(solution, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import task_assigning as ta
    from task_assigning.constraints import (
        MinimizeMakespan, get_constraints, no_missing_skills,
    )

    ta.solve(solution, constraints=get_constraints())           # defaults as-is
    ta.solve(solution, constraints=get_constraints([            # picked
        no_missing_skills, MinimizeMakespan(weight=10),
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

This domain folds four Bendable levels (hard[0], soft[0..2]) into the engine's two levels
as unit multiples (`domain.hpp`: kMissingSkillUnit = 10^9, kMakespanUnit = 10^7). Each
constraint's default weight already carries that multiple, so when passing a weight
directly, treat it as the relative share within the same level.

Names (`name`) are the C++ component names in snake_case. The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`ta.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "NoMissingSkills",
    "MinimizeMakespan",
    "CriticalPriorityTaskEndTime",
    "MajorPriorityTaskEndTime",
    "MinorPriorityTaskEndTime",
    "MinimizeUnassignedTasks",
    "no_missing_skills",
    "minimize_makespan",
    "critical_priority_task_end_time",
    "major_priority_task_end_time",
    "minor_priority_task_end_time",
    "minimize_unassigned_tasks",
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


class NoMissingSkills(Constraint):
    """Skills the task type requires that the assigned employee lacks. Default: hard, weight 10^9.

    Bendable hard[0]; it rides **above** the unassigned count in the folded hard level."""

    name = "no_missing_skills"


class MinimizeMakespan(Constraint):
    """The **square** of each employee's end time. Default: soft, weight 10^7.

    Squaring pushes toward taking work off the busiest employee rather than just
    shrinking the total. An employee without tasks ends at 0 and costs nothing. Bendable
    soft[1], so it rides above the priority end-time terms.

    The square lives inside cost() rather than in `exponent` on purpose — exponent
    is the knob that shapes the **weighted** cost, so keeping the square in the
    definition lets callers layer more shaping on top."""

    name = "minimize_makespan"


class CriticalPriorityTaskEndTime(Constraint):
    """End time of CRITICAL tasks. Default: soft, weight 4 (4x minor)."""

    name = "critical_priority_task_end_time"


class MajorPriorityTaskEndTime(Constraint):
    """End time of MAJOR tasks. Default: soft, weight 2 (2x minor)."""

    name = "major_priority_task_end_time"


class MinorPriorityTaskEndTime(Constraint):
    """End time of MINOR tasks. Default: soft, weight 1 — the baseline of the three priority rules."""

    name = "minor_priority_task_end_time"


class MinimizeUnassignedTasks(Constraint):
    """Tasks on no employee's list. Default: hard, weight 1.

    Bendable soft[0], but the engine's folded hard level carries it **below** the
    missing-skill count — the same lexicographically dominated spot as in Java.
    Unassigned tasks belong to no unit, so this is a global constraint."""

    name = "minimize_unassigned_tasks"


#: Instances at the default weights. Reference them directly when picking.
no_missing_skills = NoMissingSkills()
minimize_makespan = MinimizeMakespan()
critical_priority_task_end_time = CriticalPriorityTaskEndTime()
major_priority_task_end_time = MajorPriorityTaskEndTime()
minor_priority_task_end_time = MinorPriorityTaskEndTime()
minimize_unassigned_tasks = MinimizeUnassignedTasks()

#: Default set — same order as `ta.list_constraints()` (unit first, global after).
_DEFAULTS: list[Constraint] = [
    no_missing_skills,
    minimize_makespan,
    critical_priority_task_end_time,
    major_priority_task_end_time,
    minor_priority_task_end_time,
    minimize_unassigned_tasks,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all six default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([no_missing_skills, MinimizeMakespan(weight=10)])
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

    ``costs`` is what `ta.constraint_costs(solution)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = ta.constraint_costs(solution)
        score(raw)                     # default set
        score(raw, picked)             # picked list

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft). This domain folds
    its levels into the engine as multiples of kMissingSkillUnit / kMakespanUnit, so to
    compare with the split levels of `solution.score`, fold the same way first.
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
