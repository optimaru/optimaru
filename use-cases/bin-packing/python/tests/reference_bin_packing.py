"""The 0.1.5 pure-Python bin-packing scorer and construction, frozen as a test oracle.

This is a verbatim transcription of what ``bin_packing`` computed before the
0.2.0 C++ port (``_core.py``: ``bin_loads`` / ``constraint_costs`` /
``_first_fit`` / ``_compact``; ``constraints.py``: ``score``), kept only so the
tests can prove that the C++ port computes the same raw costs, the same default
score and the same construction. It imports nothing from the package: it works
on the problem as plain data (``BinPackingProblemModel.model_dump()`` or the
JSON a client sends: ``bin_capacity``, ``items`` with ``id`` / ``weight`` /
``bin`` / ``color``, ``conflicts`` with ``first`` / ``second``, and the 0.1.5
weight fields when present). Never import it from the package itself.

Names are the 0.1.5 catalog names; ``RENAMED`` maps them to the 0.2.0 names.

What the 0.2.0 port changes by design (documented in the package README) and
the parity tests therefore compare on totals, not on lists:

- unit granularity: 0.1.5 lists ``bin_capacity`` per bin 0..max bin,
  ``color_compatibility`` per present (bin, colour) and ``item_conflicts`` per
  listed pair; the C++ port lists every constraint per bin 0..n-1 (the
  ``used_bins_cost`` global stays a one-element list). The totals are equal for
  every assignment; at exponent 1 the scores are equal too;
- ``score`` here shapes and rounds per 0.1.5 entry; the engine shapes per bin
  (equal at exponent 1, and for integral weights the hard rounding agrees);
- ``first_fit`` can return a bin >= n for a seed that leaves an item that fits
  nowhere (a 0.1.5 bug); the C++ construction opens the lowest empty bin
  instead, so parity is asserted only where the oracle stays in range.
"""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from typing import Any

#: 0.1.5 catalog order (list_constraints()).
OLD_NAMES = ("bin_capacity", "color_compatibility", "item_conflicts", "minimize_bins")

#: 0.1.5 catalog name -> 0.2.0 name (rename_table.json).
RENAMED = {
    "bin_capacity": "bin_capacity_cost",
    "color_compatibility": "color_conflict_cost",
    "item_conflicts": "item_conflict_cost",
    "minimize_bins": "used_bins_cost",
}

#: 0.1.5 defaults: name -> (hard, model weight field). Every weight field defaulted to 1.
OLD_LEVELS = {
    "bin_capacity": (True, "capacity_weight"),
    "minimize_bins": (False, "used_bin_weight"),
    "color_compatibility": (True, "color_weight"),
    "item_conflicts": (True, "conflict_weight"),
}


def _items(problem: Mapping[str, Any]) -> list[Mapping[str, Any]]:
    return list(problem["items"])


def _bin(item: Mapping[str, Any]) -> int:
    return int(item.get("bin", -1))


def _pairs(problem: Mapping[str, Any]) -> list[tuple[str, str]]:
    pairs = []
    for pair in problem.get("conflicts") or []:
        if isinstance(pair, Mapping):
            pairs.append((pair["first"], pair["second"]))
        else:
            left, right = pair
            pairs.append((left, right))
    return pairs


def bins_used(problem: Mapping[str, Any]) -> int:
    """0.1.5 ``BinPackingProblem.bins_used``: distinct bins >= 0."""
    return len({_bin(item) for item in _items(problem) if _bin(item) >= 0})


def bin_loads(problem: Mapping[str, Any]) -> list[int]:
    """0.1.5 ``BinPackingProblem.bin_loads``: [] when nothing is assigned, else bins 0..max bin."""
    items = _items(problem)
    if not items or all(_bin(item) < 0 for item in items):
        return []
    loads = [0] * (max(_bin(item) for item in items) + 1)
    for item in items:
        if _bin(item) >= 0:
            loads[_bin(item)] += item["weight"]
    return loads


def constraint_costs(problem: Mapping[str, Any]) -> dict[str, dict[str, Any]]:
    """0.1.5 ``constraint_costs``: {old name: {hard, weight, costs}} (insertion order as in 0.1.5)."""
    items = _items(problem)
    capacity = problem["bin_capacity"]
    loads = bin_loads(problem)
    colors_by_bin: dict[int, dict[str, int]] = {}
    bins_by_id = {item["id"]: _bin(item) for item in items}
    for item in items:
        if _bin(item) >= 0 and item.get("color") is not None:
            counts = colors_by_bin.setdefault(_bin(item), {})
            counts[item["color"]] = counts.get(item["color"], 0) + 1
    weight = lambda field: problem.get(field, 1)  # noqa: E731 - the 0.1.5 model default
    return {
        "bin_capacity": {
            "hard": True,
            "weight": weight("capacity_weight"),
            "costs": [max(0, load - capacity) for load in loads],
        },
        "minimize_bins": {
            "hard": False,
            "weight": weight("used_bin_weight"),
            "costs": [sum(load > 0 for load in loads)],
        },
        "color_compatibility": {
            "hard": True,
            "weight": weight("color_weight"),
            "costs": [max(0, count - 1) for counts in colors_by_bin.values() for count in counts.values()],
        },
        "item_conflicts": {
            "hard": True,
            "weight": weight("conflict_weight"),
            "costs": [
                int(bins_by_id.get(left, -1) >= 0 and bins_by_id.get(left) == bins_by_id.get(right))
                for left, right in _pairs(problem)
            ],
        },
    }


def totals(problem: Mapping[str, Any]) -> dict[str, float]:
    """Raw total per constraint under the 0.2.0 names (exponent 1, unweighted)."""
    return {RENAMED[name]: sum(entry["costs"]) for name, entry in constraint_costs(problem).items()}


def _shape(cost: float, exponent: float) -> float:
    if exponent <= 0:
        raise ValueError("constraint exponent must be positive")
    if exponent == 1.0 or cost == 0:
        return cost
    return math.copysign(abs(cost) ** exponent, cost)


def _round_half_away(value: float) -> int:
    return int(math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5))


def score(
    costs: Mapping[str, Mapping[str, Any]],
    picks: Sequence[tuple[str, float | None, bool | None, float]] | None = None,
) -> tuple[int, float]:
    """0.1.5 ``constraints.score``: ``picks`` = (old name, weight, hard, exponent); None = all four defaults.

    Each 0.1.5 entry is shaped, weighted and (on the hard level) rounded half away from zero on its own.
    """
    if picks is None:
        picks = [(name, None, None, 1.0) for name in OLD_NAMES]
    hard = 0
    soft = 0.0
    for name, weight, is_hard, exponent in picks:
        entry = costs[name]
        weight = entry["weight"] if weight is None else weight
        is_hard = entry["hard"] if is_hard is None else is_hard
        for raw in entry["costs"]:
            value = weight * _shape(raw, exponent)
            if is_hard:
                hard += _round_half_away(value)
            else:
                soft += value
    return -hard, -soft if soft else 0.0


def default_score(problem: Mapping[str, Any]) -> tuple[int, float]:
    """0.1.5 ``calculate_score(problem)`` at the default weights."""
    return score(constraint_costs(problem))


def compact(assignments: Sequence[int]) -> list[int]:
    """0.1.5 ``_compact``: relabel bins by first appearance in item order."""
    remap: dict[int, int] = {}
    return [remap.setdefault(bin_index, len(remap)) for bin_index in assignments]


def descending_order(weights: Sequence[int]) -> list[int]:
    """0.1.5 start-0 construction order: weight descending, then index."""
    return sorted(range(len(weights)), key=lambda index: (-weights[index], index))


def first_fit(
    weights: Sequence[int], capacity: int, order: Sequence[int], seed: Sequence[int] | None = None
) -> list[int]:
    """0.1.5 ``_first_fit`` (actually best fit): place every item of `order` that `seed` leaves at -1."""
    assignments = [-1] * len(weights) if seed is None else list(seed)
    loads = [0] * (max(assignments, default=-1) + 1)
    for item_index, bin_index in enumerate(assignments):
        if bin_index >= 0:
            loads[bin_index] += weights[item_index]
    for item_index in order:
        if assignments[item_index] >= 0:
            continue
        weight = weights[item_index]
        candidates = [
            (capacity - (load + weight), bin_index) for bin_index, load in enumerate(loads) if load + weight <= capacity
        ]
        if candidates:
            _, selected = min(candidates)
        else:
            selected = len(loads)
            loads.append(0)
        assignments[item_index] = selected
        loads[selected] += weight
    return assignments


def construct(problem: Mapping[str, Any], *, warm_start: bool | None = None) -> list[int]:
    """What a 0.1.5 zero-budget solve returned: the start-0 construction (a seed completed when warm)."""
    items = _items(problem)
    weights = [item["weight"] for item in items]
    warm = problem.get("warm_start", False) if warm_start is None else warm_start
    seed = [_bin(item) for item in items] if warm else None
    return first_fit(weights, problem["bin_capacity"], descending_order(weights), seed)
