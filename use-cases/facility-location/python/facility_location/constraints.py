"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`fl.solve(problem)`) enables all three constraints at their default
weights. This module is the alternative path next to it: pick constraints, change
weights, flip hard/soft, shape the cost, and pass the list to
`solve(problem, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import facility_location as fl
    from facility_location.constraints import (
        DistanceFromFacilityCost, FacilityCapacityCost, FacilitySetupCost, get_constraints,
    )

    fl.solve(problem, constraints=get_constraints())               # the defaults as-is
    fl.solve(problem, constraints=[                                 # picked
        FacilityCapacityCost(), DistanceFromFacilityCost(weight=10),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``facility_capacity_cost``
is ``FacilityCapacityCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (FacilityCapacityCost 1, FacilitySetupCost 2,
               DistanceFromFacilityCost 6 — the distance 6 is the
               ConstraintWeightOverrides value of the Java solution). The problem
               carries no weights; ``fl.constraint_info()`` reports the defaults.
- ``hard``     ``True`` = hard, ``False`` = soft, ``None`` = the default level.
- ``exponent`` sign-preserving power shaping per facility: ``weight * sign(c) *
               |c| ** exponent``. 2.0 = squared, so concentrated violations grow
               much costlier.
- ``offset``   (keyword) shifts that shaping per facility; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a facility below the offset — an unused
               one (cost 0) included — lowers the penalty.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs and the picks add up. Distance plus its square beyond 500 m per
facility::

    fl.solve(problem, constraints=[
        FacilityCapacityCost(),
        FacilitySetupCost(),
        DistanceFromFacilityCost(),
        DistanceFromFacilityCost(weight=0.01, exponent=2, offset=500),
    ])

``fl.constraint_costs(problem, picks)`` keys the second pick
``"distance_from_facility_cost#2"``.

Names (`name`) are the C++ component names in snake_case
(``FacilityCapacityCost`` -> ``facility_capacity_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`fl.list_constraints()`.
"""

from ._constraint import Constraint, Score, score_costs, select

__all__ = [
    "Constraint",
    "Score",
    "score",
    "FacilityCapacityCost",
    "FacilitySetupCost",
    "DistanceFromFacilityCost",
    "facility_capacity_cost",
    "facility_setup_cost",
    "distance_from_facility_cost",
    "get_constraints",
]


class FacilityCapacityCost(Constraint):
    """Demand above capacity at a facility in use. Default: hard, weight 1."""

    name = "facility_capacity_cost"
    label_en = "Facility capacity"
    label_ko = "시설 용량"
    description_en = (
        "Demand assigned to a facility beyond its capacity, summed over the facilities in use. "
        "One unit of cost is one unit of demand over capacity; 0 means every facility is within its capacity."
    )
    description_ko = (
        "사용 중인 시설마다 배정된 수요 가운데 용량을 넘는 양을 모두 더합니다. "
        "초과 수요 1단위가 비용 1이며, 0이면 모든 시설이 용량 안에 있습니다."
    )


class FacilitySetupCost(Constraint):
    """Setup cost of every facility serving at least one consumer. Default: soft, weight 2.

    A step function of "is this facility used", so moving one consumer cannot
    cross it — that is why the domain carries a separate CloseFacilityMove."""

    name = "facility_setup_cost"
    label_en = "Facility setup cost"
    label_ko = "시설 개설 비용"
    description_en = (
        "The setup cost of every facility that serves at least one consumer, paid once per facility. "
        "0 means no facility is open."
    )
    description_ko = "고객을 한 명 이상 맡은 시설마다 개설 비용을 한 번씩 더합니다. 0이면 문을 연 시설이 없습니다."


class DistanceFromFacilityCost(Constraint):
    """Distance (m) from each assigned consumer to its facility. Default: soft, weight 6."""

    name = "distance_from_facility_cost"
    label_en = "Distance from facility"
    label_ko = "시설까지의 거리"
    description_en = (
        "The distance in meters from each assigned consumer to its facility, summed. "
        "0 means every assigned consumer stands at its facility."
    )
    description_ko = (
        "배정된 고객마다 담당 시설까지의 거리(미터)를 모두 더합니다. "
        "0이면 배정된 모든 고객이 담당 시설과 같은 곳에 있습니다."
    )


#: Ready-made default instances (``facility_capacity_cost`` is ``FacilityCapacityCost()``).
facility_capacity_cost = FacilityCapacityCost()
facility_setup_cost = FacilitySetupCost()
distance_from_facility_cost = DistanceFromFacilityCost()

# The default set, in list_constraints() order.
_DEFAULTS = (facility_capacity_cost, facility_setup_cost, distance_from_facility_cost)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all three default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, list the others as they are::

        get_constraints([FacilityCapacityCost(), FacilitySetupCost(), DistanceFromFacilityCost(weight=10)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) raises ValueError; a constraint named several times is
    scored once per pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `fl.constraint_costs(problem)` returns: per constraint, the
    **raw** cost of each facility (before weight, exponent, offset and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = fl.constraint_costs(problem)
        score(raw)                                    # default set
        score(raw, [FacilityCapacityCost(), DistanceFromFacilityCost(weight=10)])
        score(raw, [DistanceFromFacilityCost(), DistanceFromFacilityCost(exponent=2, offset=500)])

    No solver and no search are involved. The arithmetic matches the engine —
    each facility contributes ``weight * shape(cost, exponent, offset, clip)`` and
    hard contributions are **rounded per facility** before summing (the same order
    as the engine; rounding the total afterwards gives different values). A second
    pick of a constraint reads the same raw costs with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    return score_costs(costs, get_constraints(constraints))
