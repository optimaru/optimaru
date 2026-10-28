"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`fl.solve(problem)`) enables all three constraints at the quickstart weights.
This module is the alternative path next to it: pick constraints, change weights, flip
hard/soft, and pass the list to `solve(problem, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import facility_location as fl
    from facility_location.constraints import (
        DistanceFromFacility, capacity, get_constraints,
    )

    fl.solve(problem, constraints=get_constraints())              # defaults as-is
    fl.solve(problem, constraints=get_constraints([              # picked
        capacity, DistanceFromFacility(weight=10),
    ]))

Default instances (`capacity`, `setup_cost`, `distance`) live at module level, so
you can also pass a list of just a few. Only listed constraints are scored — leave
one out and its term disappears from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = component default
               (facilityCapacity 1, setup cost 2, distance 6 — the distance 6
               is the ConstraintWeightOverrides value of the Java solution).
- ``hard``     ``True``/``False`` flips the default level; ``None`` keeps it.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. 2.0 = squared, so concentrated violations grow much costlier.

Names (`name`) are the C++ component names in snake_case. The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`fl.list_constraints()`.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "FacilityCapacity",
    "FacilitySetupCost",
    "DistanceFromFacility",
    "capacity",
    "setup_cost",
    "distance",
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


class FacilityCapacity(Constraint):
    """Demand above capacity at a facility in use. Default: hard, weight 1."""

    name = "facility_capacity"


class FacilitySetupCost(Constraint):
    """Setup cost of every facility serving at least one consumer. Default: soft, weight 2.

    A step function of "is this facility used", so moving one consumer cannot
    cross it — that is why the domain carries a separate CloseFacilityMove."""

    name = "facility_setup_cost"


class DistanceFromFacility(Constraint):
    """Distance (m) from each assigned consumer to its facility. Default: soft, weight 6."""

    name = "distance_from_facility"


#: Instances at the default weights. Reference them directly when picking.
capacity = FacilityCapacity()
setup_cost = FacilitySetupCost()
distance = DistanceFromFacility()


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all three default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([capacity, setup_cost, DistanceFromFacility(weight=10)])
    """
    if constraints is None:
        return [capacity, setup_cost, distance]
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

    ``costs`` is what `fl.constraint_costs(problem)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = fl.constraint_costs(problem)
        score(raw)                                    # default set
        score(raw, [capacity, DistanceFromFacility(weight=10)])

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
                # like llround, not banker's rounding).
                hard += int(math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5))
            else:
                soft += value
    return Score(-hard, -soft if soft != 0.0 else 0.0)
