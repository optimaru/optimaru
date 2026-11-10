"""An independent pure-Python quadratic-assignment scorer, kept as a test oracle.

A plain implementation of the constraint costs (``constraint_costs``), the score
(``score_costs`` / ``calculate_score``) and ``total_cost``, kept only so the tests
can prove that the C++ engine computes the same raw costs and the same default
score. It imports nothing
from the package: it works on the problem as plain data
(``QuadraticAssignmentProblemModel.model_dump()`` or the JSON a client sends).
Never import it from the package itself.

It uses its own constraint names; ``ENGINE_NAME`` maps them to the engine's names.

Semantics (the C++ engine differs only where noted in the package README):

- ``constraint_costs`` needs a complete assignment (every location; a missing
  one raises ``KeyError``), lists ``fixed_assignments`` once per FIXED location
  (in ``fixed_assignments`` order) and ``forbidden_assignments`` once per entry of
  ``assignment`` (in its order); the C++ engine lists both per location, in
  location order, with 0 for the locations that are not fixed / not involved —
  the same non-zero terms, so the totals agree for every exponent;
- a non-injective assignment (one facility at two locations) is scored here; the
  C++ engine cannot represent it (``to_core()`` raises ``ValueError``);
- duplicate ``forbidden_assignments`` rows collapse (a set), as in the C++ engine;
- ``score_costs`` rounds the summed hard total once; the C++ engine rounds per
  unit (location). They differ only when a hard constraint's weighted, shaped
  per-location value is fractional: a fractional hard weight, or an exponent
  != 1 on ``location_capacity_cost`` (costs ``[2, 2]`` at exponent 0.5: ``-2`` in
  the engine, ``-3`` here).
"""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from typing import Any

#: The reference catalog, in the engine's list_constraints() order.
REFERENCE_NAMES = (
    "fixed_assignments",
    "forbidden_assignments",
    "location_capacities",
    "minimize_assignment_cost",
    "minimize_flow_distance",
)

#: Defaults: name -> (hard, weight).
REFERENCE_DEFAULTS = {
    "fixed_assignments": (True, 1.0),
    "forbidden_assignments": (True, 1.0),
    "location_capacities": (True, 1.0),
    "minimize_assignment_cost": (False, 1.0),
    "minimize_flow_distance": (False, 1.0),
}

#: Reference name -> engine name.
ENGINE_NAME = {
    "fixed_assignments": "pinned_assignments_cost",
    "forbidden_assignments": "forbidden_assignments_cost",
    "location_capacities": "location_capacity_cost",
    "minimize_assignment_cost": "assignment_cost",
    "minimize_flow_distance": "flow_distance_cost",
}


def constraint_costs(problem: Mapping[str, Any]) -> dict[str, list[int]]:
    """The raw costs: ``{reference name: [raw terms]}``."""
    facilities = list(problem["facilities"])
    locations = list(problem["locations"])
    flows = problem["flows"]
    distances = problem["distances"]
    assignment_costs = problem.get("assignment_costs")
    assignment = dict(problem.get("assignment") or {})
    fixed_assignments = dict(problem.get("fixed_assignments") or {})
    forbidden_rows = list(problem.get("forbidden_assignments") or [])
    facility_demands = dict(problem.get("facility_demands") or {})
    location_capacities = dict(problem.get("location_capacities") or {})

    facility_index = {name: index for index, name in enumerate(facilities)}
    permutation = [facility_index[assignment[location]] for location in locations]
    cost = sum(
        distances[i][j] * flows[permutation[i]][permutation[j]]
        for i in range(len(permutation))
        for j in range(len(permutation))
    )
    assignment_cost = 0
    if assignment_costs is not None:
        assignment_cost = sum(
            assignment_costs[facility_index[assignment[location]]][location_index]
            for location_index, location in enumerate(locations)
        )
    forbidden = {(row["location"], row["facility"]) for row in forbidden_rows}
    return {
        "fixed_assignments": [
            int(assignment.get(location) != facility) for location, facility in fixed_assignments.items()
        ],
        "forbidden_assignments": [int((location, facility) in forbidden) for location, facility in assignment.items()],
        "location_capacities": [
            max(
                0,
                facility_demands.get(assignment[location], 0)
                - location_capacities.get(location, facility_demands.get(assignment[location], 0)),
            )
            for location in locations
        ],
        "minimize_assignment_cost": [assignment_cost],
        "minimize_flow_distance": [cost],
    }


def score_costs(costs: Mapping[str, Sequence[float]], selection: Sequence[tuple] | None = None) -> tuple[int, float]:
    """The score of raw costs: ``(hard, soft)``.

    ``selection`` is a list of ``(reference name, weight | None, hard | None, exponent)``;
    None = every default at its default weight and level, exponent 1.
    """
    if selection is None:
        selection = [(name, None, None, 1.0) for name in REFERENCE_NAMES]
    if not selection:
        raise ValueError("constraints must not be empty (pass None for defaults)")
    hard = 0.0
    soft = 0.0
    for name, weight, is_hard, exponent in selection:
        if name not in costs:
            raise ValueError(f"unknown constraint: {name}")
        raw = costs[name]
        default_hard, default_weight = REFERENCE_DEFAULTS[name]
        weight = default_weight if weight is None else weight
        is_hard = default_hard if is_hard is None else is_hard
        if exponent <= 0:
            raise ValueError("constraint exponent must be positive")
        total = sum(math.copysign(abs(value) ** exponent, value) for value in raw)
        if is_hard:
            hard += weight * total
        else:
            soft += weight * total
    rounded_hard = int(math.floor(hard + 0.5) if hard >= 0 else math.ceil(hard - 0.5))
    return -rounded_hard, -soft if soft else 0.0


def calculate_score(problem: Mapping[str, Any], selection: Sequence[tuple] | None = None) -> tuple[int, float]:
    """The score of a problem with an assignment."""
    return score_costs(constraint_costs(problem), selection)


def total_cost(problem: Mapping[str, Any]) -> int:
    """``total_cost`` = flow-distance plus direct assignment cost."""
    costs = constraint_costs(problem)
    return costs["minimize_flow_distance"][0] + costs["minimize_assignment_cost"][0]
