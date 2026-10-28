"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`op.solve(solution)`) enables all four constraints at the quickstart weights.
This module is the alternative path next to it: pick constraints, change weights, flip
hard/soft, and pass the list to `solve(solution, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import order_picking as op
    from order_picking.constraints import (
        MinimizeOrderSplitByTrolley, get_constraints, required_number_of_buckets,
    )

    op.solve(solution, constraints=get_constraints())           # defaults as-is
    op.solve(solution, constraints=get_constraints([            # picked
        required_number_of_buckets, MinimizeOrderSplitByTrolley(weight=100),
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
`op.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.

Two knobs do not apply on the plugin path: `parallel_solver_count`
(a caller-owned calculator cannot be cloned into workers) and `order_consolidate_moves`
(that operator reads the monolithic calculator's live volume table). Details are in
the comments of `cpp/include/op/solver.hpp`.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "RequiredNumberOfBuckets",
    "MinimizeDistanceFromPreviousPickTask",
    "MinimizeDistanceFromLastPickTaskToPathOrigin",
    "MinimizeOrderSplitByTrolley",
    "required_number_of_buckets",
    "minimize_distance_from_previous_pick_task",
    "minimize_distance_from_last_pick_task_to_path_origin",
    "minimize_order_split_by_trolley",
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


class RequiredNumberOfBuckets(Constraint):
    """Buckets a trolley is short of. Default: hard, weight 1.

    Orders never share buckets, so bucket counts round up the volume of **each
    order** on the trolley — that rounding is exactly why splitting an order over
    several trolleys wastes capacity."""

    name = "required_number_of_buckets"


class MinimizeDistanceFromPreviousPickTask(Constraint):
    """Travel distance from each pick to the next. Default: soft, weight 1.

    For the first pick, the trolley's parking spot stands in as the "previous location"."""

    name = "minimize_distance_from_previous_pick_task"


class MinimizeDistanceFromLastPickTaskToPathOrigin(Constraint):
    """Distance from the last pick back to the starting point. Default: soft, weight 1.

    An empty trolley never leaves, so it costs nothing."""

    name = "minimize_distance_from_last_pick_task_to_path_origin"


class MinimizeOrderSplitByTrolley(Constraint):
    """Number of (order, trolley) pairs. Default: soft, weight 1000.

    An order shared by three trolleys costs three times one carried by a single
    trolley. Deliberately expensive relative to the distance terms — orders arriving
    in pieces become a packing problem downstream."""

    name = "minimize_order_split_by_trolley"


#: Instances at the default weights. Reference them directly when picking.
required_number_of_buckets = RequiredNumberOfBuckets()
minimize_distance_from_previous_pick_task = MinimizeDistanceFromPreviousPickTask()
minimize_distance_from_last_pick_task_to_path_origin = MinimizeDistanceFromLastPickTaskToPathOrigin()
minimize_order_split_by_trolley = MinimizeOrderSplitByTrolley()

#: Default set — same order as `op.list_constraints()`.
_DEFAULTS: list[Constraint] = [
    required_number_of_buckets,
    minimize_distance_from_previous_pick_task,
    minimize_distance_from_last_pick_task_to_path_origin,
    minimize_order_split_by_trolley,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all four default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([required_number_of_buckets, MinimizeOrderSplitByTrolley(weight=100)])
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

    ``costs`` is what `op.constraint_costs(solution)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = op.constraint_costs(solution)
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
