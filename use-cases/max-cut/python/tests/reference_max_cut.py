"""An independent pure-Python max-cut scorer and start state, kept as a test oracle.

``constraint_costs`` / ``calculate_score``, ``score_costs`` and
``start_partition`` are written without the C++ domain, so the tests can prove
that the C++ domain computes the same raw costs, the same default score and the
same start partition. It imports nothing from the package: it works on the
problem as plain data (``MaxCutProblemModel.model_dump()`` or the JSON a client
sends). Never import it from the package itself.

The reference uses its own constraint names; ``ENGINE_NAME`` maps them to the
engine names.

Where it differs from the C++ domain by design:

- ``constraint_costs`` lists one 0/1 term per fixed vertex / same-side pair /
  different-side pair; the C++ domain reports one term per SIDE (unit), whose
  sum equals the sum of these terms on every complete partition;
- ``score_costs`` shapes every TERM with the exponent and rounds the hard total
  once; the C++ domain shapes each unit's cost and rounds per unit (identical
  at exponent 1 and integral weights);
- a vertex missing from ``partition`` counts as a side of its own here (cut,
  pairs) and as side 0 for the sizes; the package refuses to score a partial
  partition at the model level;
- ``maximize_cut_weight`` with ``hard=True`` gives a positive hard score here;
  the engine refuses a hard reward.
"""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from typing import Any

#: The reference catalog order (the engine's list_constraints() order).
REFERENCE_NAMES = (
    "fixed_partition",
    "same_partition_pairs",
    "different_partition_pairs",
    "partition_size_bounds",
    "partition_balance",
    "maximize_cut_weight",
)

#: The reference defaults: name -> (hard, weight).
REFERENCE_DEFAULTS = {
    "fixed_partition": (True, 1.0),
    "same_partition_pairs": (True, 1.0),
    "different_partition_pairs": (True, 1.0),
    "partition_size_bounds": (True, 1.0),
    "partition_balance": (False, 0.0),
    "maximize_cut_weight": (False, 1.0),
}

#: Reference name -> engine name, one entry per constraint.
ENGINE_NAME = {
    "fixed_partition": "fixed_partition_cost",
    "same_partition_pairs": "same_partition_pairs_cost",
    "different_partition_pairs": "different_partition_pairs_cost",
    "partition_size_bounds": "partition_size_bounds_cost",
    "partition_balance": "partition_imbalance_cost",
    "maximize_cut_weight": "cut_weight_cost",
}


def _pairs(problem: Mapping[str, Any], key: str) -> list[tuple[str, str]]:
    return [(pair["first"], pair["second"]) for pair in problem.get(key, [])]


def constraint_costs(problem: Mapping[str, Any]) -> dict[str, list]:
    """The reference ``constraint_costs``: ``{reference name: [raw terms]}``."""
    vertices = list(problem["vertices"])
    partition = dict(problem.get("partition", {}))
    edges = list(problem.get("edges", []))
    fixed = dict(problem.get("fixed_partition", {}))
    cut = sum(edge.get("weight", 1) for edge in edges if partition.get(edge["source"]) != partition.get(edge["target"]))
    side_one = sum(partition.get(vertex, 0) == 1 for vertex in vertices)
    side_zero = len(vertices) - side_one
    min_size = problem.get("min_partition_size")
    max_size = problem.get("max_partition_size")
    minimum = 0 if min_size is None else min_size
    maximum = len(vertices) if max_size is None else max_size
    return {
        "fixed_partition": [int(partition.get(vertex) != side) for vertex, side in fixed.items()],
        "same_partition_pairs": [
            int(partition.get(first) != partition.get(second))
            for first, second in _pairs(problem, "same_partition_pairs")
        ],
        "different_partition_pairs": [
            int(partition.get(first) == partition.get(second))
            for first, second in _pairs(problem, "different_partition_pairs")
        ],
        "partition_size_bounds": [
            max(0, minimum - side_zero) + max(0, side_zero - maximum),
            max(0, minimum - side_one) + max(0, side_one - maximum),
        ],
        "partition_balance": [abs(side_zero - side_one)],
        "maximize_cut_weight": [-cut],
    }


def cut_weight(problem: Mapping[str, Any]) -> int:
    """The reference ``calculate_score``'s ``problem.cut_weight``."""
    return -constraint_costs(problem)["maximize_cut_weight"][0]


def score_costs(
    costs: Mapping[str, Sequence[float]],
    constraints: Sequence[Mapping[str, Any]] | None = None,
) -> tuple[int, float]:
    """The reference ``score_costs``: ``(hard, soft)`` for a selection.

    ``constraints`` is a list of ``{"name": <reference name>, "weight": float | None,
    "hard": bool | None, "exponent": float}`` (None = the reference defaults, every
    constraint at its default level and weight).
    """
    selected = (
        [{"name": name, "weight": None, "hard": None, "exponent": 1.0} for name in REFERENCE_NAMES]
        if constraints is None
        else list(constraints)
    )
    if not selected:
        raise ValueError("constraints must not be empty (pass None for defaults)")
    hard = 0.0
    soft = 0.0
    for constraint in selected:
        name = constraint["name"]
        if name not in costs:
            raise ValueError(f"unknown constraint: {name}")
        raw = costs[name]
        default_hard, default_weight = REFERENCE_DEFAULTS[name]
        weight = default_weight if constraint.get("weight") is None else constraint["weight"]
        is_hard = default_hard if constraint.get("hard") is None else constraint["hard"]
        exponent = constraint.get("exponent", 1.0)
        if exponent <= 0:
            raise ValueError("constraint exponent must be positive")
        total = sum(math.copysign(abs(value) ** exponent, value) for value in raw)
        if is_hard:
            hard += weight * total
        else:
            soft += weight * total
    rounded_hard = int(math.floor(hard + 0.5) if hard >= 0 else math.ceil(hard - 0.5))
    return -rounded_hard, -soft if soft else 0.0


def calculate_score(
    problem: Mapping[str, Any], constraints: Sequence[Mapping[str, Any]] | None = None
) -> tuple[int, float]:
    """The reference ``calculate_score`` (without the write-back onto the model)."""
    return score_costs(constraint_costs(problem), constraints)


def start_partition(problem: Mapping[str, Any]) -> dict[str, int]:
    """The start state the reference builds before searching.

    Vertex i takes side i % 2, a fixed vertex its fixed side, and with
    ``warm_start`` every ``partition`` entry overrides both (a seed against a
    fixed side is kept: ``with_initial_solution`` refuses one, a raw body may
    carry one).
    """
    vertices = list(problem["vertices"])
    initial = [index % 2 for index in range(len(vertices))]
    index_of = {vertex: index for index, vertex in enumerate(vertices)}
    for vertex, side in dict(problem.get("fixed_partition", {})).items():
        initial[index_of[vertex]] = side
    if problem.get("warm_start", False):
        for vertex, side in dict(problem.get("partition", {})).items():
            initial[index_of[vertex]] = side
    return dict(zip(vertices, initial, strict=True))
