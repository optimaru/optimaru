"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`quadratic_assignment.solve(problem)`) scores every default
constraint at its default weight. This module is the alternative path next to
it: pick constraints, change weights, change levels, and pass the list to
`solve(problem, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import quadratic_assignment as qap
    from quadratic_assignment.constraints import FlowDistanceCost, get_constraints

    qap.solve(problem, constraints=get_constraints())                        # defaults as-is
    qap.solve(problem, constraints=[*get_constraints()[:-1], FlowDistanceCost(weight=2)])

Default instances (`pinned_assignments_cost`, `forbidden_assignments_cost`,
`location_capacity_cost`, `assignment_cost`, `flow_distance_cost`) live at
module level, so you can also pass a list of just a few. Only listed
constraints are scored — leave one out and its term disappears from the score.

Every spec takes ``(weight=None, level=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (1 for every quadratic-assignment constraint). The problem
               carries no weights; ``quadratic_assignment.constraint_info()``
               reports the defaults.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level.
- ``exponent`` sign-preserving power shaping ``weight * sign(c) * |c| ** exponent``:
               per location for the three side constraints (pinned, forbidden,
               capacity), on the single total for ``assignment_cost`` and
               ``flow_distance_cost`` — exactly the terms 0.1.x shaped.

Names (`name`) are the C++ component names in snake_case
(``PinnedAssignmentsCost`` -> ``pinned_assignments_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`quadratic_assignment.list_constraints()`.
"""

from ._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "PinnedAssignmentsCost",
    "ForbiddenAssignmentsCost",
    "LocationCapacityCost",
    "AssignmentCost",
    "FlowDistanceCost",
    "pinned_assignments_cost",
    "forbidden_assignments_cost",
    "location_capacity_cost",
    "assignment_cost",
    "flow_distance_cost",
    "get_constraints",
]


class PinnedAssignmentsCost(Constraint):
    """Fixed locations that do not hold their facility. Default: hard, weight 1."""

    name = "pinned_assignments_cost"
    label_en = "Pinned assignments"
    label_ko = "고정 배치"
    description_en = (
        "Locations listed in fixed_assignments that do not hold their facility (an empty location counts too); "
        "two locations pinned to the same facility cost at least 1. One unit of cost is one pin not kept; "
        "0 means every fixed location holds its facility."
    )
    description_ko = (
        "fixed_assignments에 지정된 위치 가운데 해당 시설이 놓이지 않은 위치의 수이며, 비어 있는 위치도 포함합니다. "
        "같은 시설을 두 위치에 고정하면 비용은 최소 1입니다. 지켜지지 않은 고정 배치 하나가 비용 1이며, "
        "0이면 모든 고정 위치에 해당 시설이 놓여 있습니다."
    )


class ForbiddenAssignmentsCost(Constraint):
    """Facilities placed at a location they are forbidden at. Default: hard, weight 1."""

    name = "forbidden_assignments_cost"
    label_en = "Forbidden assignments"
    label_ko = "금지 배치"
    description_en = (
        "Facilities standing at a location a forbidden_assignments row forbids them; a pair listed twice counts "
        "once. One unit of cost is one forbidden placement; 0 means no forbidden pair is used."
    )
    description_ko = (
        "forbidden_assignments가 금지한 위치에 놓인 시설의 수이며, 같은 시설-위치 쌍이 두 번 나열되어도 한 번만 "
        "셉니다. 금지된 배치 하나가 비용 1이며, 0이면 금지된 배치가 하나도 없습니다."
    )


class LocationCapacityCost(Constraint):
    """Facility demand above a location's capacity. Default: hard, weight 1."""

    name = "location_capacity_cost"
    label_en = "Location capacity"
    label_ko = "위치 용량"
    description_en = (
        "Demand of the facility at each location beyond the location's capacity, summed over the locations "
        "with a capacity; a facility without a facility_demands entry has demand 0. One unit of cost is one "
        "unit of demand over capacity; 0 means every location is within its capacity."
    )
    description_ko = (
        "용량이 정해진 위치마다 그 위치에 놓인 시설의 수요가 용량을 넘는 만큼을 모두 더한 값이며, "
        "facility_demands에 없는 시설의 수요는 0으로 봅니다. 초과 수요 1단위가 비용 1이며, 0이면 모든 위치가 "
        "용량을 넘지 않습니다."
    )


class AssignmentCost(Constraint):
    """Direct facility-to-location assignment cost. Default: soft, weight 1."""

    name = "assignment_cost"
    label_en = "Assignment cost"
    label_ko = "배치 비용"
    description_en = (
        "The direct cost of placing each facility at its location (assignment_costs), summed over the "
        "facilities; 0 without a cost matrix. One unit of cost is one unit of that cost; the matrix may hold "
        "negative entries, which lower the cost."
    )
    description_ko = (
        "각 시설을 현재 위치에 두는 직접 비용(assignment_costs)을 모든 시설에 대해 더한 값이며, 비용 행렬이 "
        "없으면 0입니다. 행렬 값 1이 비용 1이며, 음수 값은 그만큼 비용을 줄입니다."
    )


class FlowDistanceCost(Constraint):
    """Facility flow multiplied by the distance between their locations. Default: soft, weight 1."""

    name = "flow_distance_cost"
    label_en = "Flow-distance cost"
    label_ko = "흐름-거리 비용"
    description_en = (
        "The quadratic objective: for every ordered pair of facilities, the flow between them times the "
        "distance between their locations, summed. One unit of cost is one unit of flow carried over one unit "
        "of distance; 0 means no flow travels any distance."
    )
    description_ko = (
        "이차 목적 함수입니다. 순서가 있는 모든 시설 쌍에 대해 두 시설 사이의 흐름과 두 시설이 놓인 위치 사이의 "
        "거리를 곱해 더합니다. 흐름 1단위가 거리 1단위를 이동할 때마다 비용 1이며, 0이면 어떤 흐름도 거리를 "
        "이동하지 않습니다."
    )


#: Instances at the default weights (weight=None). Reference them directly when picking.
pinned_assignments_cost = PinnedAssignmentsCost()
forbidden_assignments_cost = ForbiddenAssignmentsCost()
location_capacity_cost = LocationCapacityCost()
assignment_cost = AssignmentCost()
flow_distance_cost = FlowDistanceCost()

# The default set, in list_constraints() order.
_DEFAULTS = (
    pinned_assignments_cost,
    forbidden_assignments_cost,
    location_capacity_cost,
    assignment_cost,
    flow_distance_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, every default constraint.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([*get_constraints()[:-1], FlowDistanceCost(weight=2)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) and a constraint named twice raise ValueError.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `quadratic_assignment.constraint_costs(problem)` returns: per
    constraint its **raw** costs (one per location for the side constraints, one
    total for the objectives), before weight, exponent and level rounding. Fetch it
    once and rescore with different weights without re-solving::

        raw = quadratic_assignment.constraint_costs(problem)
        score(raw)                                               # default set
        score(raw, [pinned_assignments_cost, FlowDistanceCost(weight=2)])

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine; rounding the total afterwards gives different values for fractional
    hard weights).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    return score_costs(costs, get_constraints(constraints))
