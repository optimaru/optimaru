"""The 0.1.5 pure-Python knapsack scorer and greedy construction, frozen as a test oracle.

This is a verbatim transcription of what ``knapsack`` computed before the 0.2.0
C++ port (``__init__.py``: ``constraint_costs`` / ``calculate_score``;
``common.py``: ``score_costs``; ``models.py``: ``_greedy_selection``), kept only
so the tests can prove that the C++ port computes the same raw costs, the same
default score and the same greedy start. It imports nothing from the package:
it works on the problem as plain data (``KnapsackProblemModel.model_dump()`` or
the JSON a client sends). Never import it from the package itself.

Names are the 0.1.5 catalog names; ``RENAMED`` maps them to the 0.2.0 names.

Semantics kept as they were (the C++ port differs only where noted in the
package README):

- ``constraint_costs`` lists one term per resource / mandatory entry /
  forbidden entry / group / dependency pair; the C++ port reports one total per
  constraint (a global constraint), equal to the sum of these terms;
- ``score_costs`` shapes every TERM with the exponent and rounds the hard total
  once; the C++ port shapes each constraint's total and rounds per constraint
  (identical at exponent 1 and integral weights);
- ``maximize_value`` with ``hard=True`` gives a positive hard score here; 0.2.0
  refuses a hard reward.
"""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from typing import Any

#: 0.1.5 catalog order (list_constraints()).
OLD_NAMES = (
    "capacity",
    "resource_capacities",
    "mandatory_items",
    "forbidden_items",
    "selection_group_bounds",
    "item_dependencies",
    "maximize_value",
)

#: 0.1.5 defaults: name -> (hard, weight).
OLD_DEFAULTS = {
    "capacity": (True, 1.0),
    "resource_capacities": (True, 1.0),
    "mandatory_items": (True, 1.0),
    "forbidden_items": (True, 1.0),
    "selection_group_bounds": (True, 1.0),
    "item_dependencies": (True, 1.0),
    "maximize_value": (False, 1.0),
}

#: 0.1.5 catalog name -> 0.2.0 name (the renames listed in the package README, section 0.2.0).
RENAMED = {
    "capacity": "knapsack_capacity_cost",
    "resource_capacities": "resource_capacity_cost",
    "mandatory_items": "mandatory_items_cost",
    "forbidden_items": "forbidden_items_cost",
    "selection_group_bounds": "selection_group_bounds_cost",
    "item_dependencies": "item_dependency_cost",
    "maximize_value": "selected_value_cost",
}


def _items(problem: Mapping[str, Any]) -> list[Mapping[str, Any]]:
    return list(problem.get("items", []))


def constraint_costs(problem: Mapping[str, Any]) -> dict[str, list]:
    """0.1.5 ``knapsack.constraint_costs``: ``{old name: [raw terms]}``."""
    items = _items(problem)
    weight = sum(item["weight"] for item in items if item.get("selected", False))
    value = sum(item["value"] for item in items if item.get("selected", False))
    selected = {item["id"] for item in items if item.get("selected", False)}
    resource_capacities = dict(problem.get("resource_capacities", {}))
    usage = {
        resource: sum(
            dict(item.get("resource_usage", {})).get(resource, 0) for item in items if item.get("selected", False)
        )
        for resource in resource_capacities
    }
    group_costs = []
    for group in problem.get("selection_groups", []):
        count = len(selected.intersection(group["item_ids"]))
        maximum = len(group["item_ids"]) if group.get("max_selected") is None else group["max_selected"]
        group_costs.append(max(0, group.get("min_selected", 0) - count) + max(0, count - maximum))
    return {
        "capacity": [max(0, weight - problem["capacity"])],
        "resource_capacities": [
            max(0, usage[resource] - capacity) for resource, capacity in resource_capacities.items()
        ],
        "mandatory_items": [int(item_id not in selected) for item_id in problem.get("mandatory_items", [])],
        "forbidden_items": [int(item_id in selected) for item_id in problem.get("forbidden_items", [])],
        "selection_group_bounds": group_costs,
        "item_dependencies": [
            int(item["id"] in selected and required not in selected)
            for item in items
            for required in item.get("requires", [])
        ],
        "maximize_value": [-value],
    }


def score_costs(costs: Mapping[str, Sequence[float]], selection: Sequence[tuple] | None = None) -> tuple[int, float]:
    """0.1.5 ``common.score_costs``: ``(hard, soft)``.

    ``selection`` is a list of ``(old name, weight | None, hard | None, exponent)``;
    None = every default at its default weight and level, exponent 1.
    """
    if selection is None:
        selection = [(name, None, None, 1.0) for name in OLD_NAMES]
    if not selection:
        raise ValueError("constraints must not be empty (pass None for defaults)")
    hard = 0.0
    soft = 0.0
    for name, weight, is_hard, exponent in selection:
        if name not in costs:
            raise ValueError(f"unknown constraint: {name}")
        raw = costs[name]
        default_hard, default_weight = OLD_DEFAULTS[name]
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
    """0.1.5 ``knapsack.calculate_score`` (the score part)."""
    return score_costs(constraint_costs(problem), selection)


def totals(problem: Mapping[str, Any]) -> tuple[int, int]:
    """0.1.5 ``calculate_score`` side effects: ``(total_weight, total_value)`` of the selection."""
    items = _items(problem)
    return (
        sum(item["weight"] for item in items if item.get("selected", False)),
        sum(item["value"] for item in items if item.get("selected", False)),
    )


def greedy_selection(problem: Mapping[str, Any], decided: Mapping[str, bool] | None = None) -> list[bool]:
    """0.1.5 ``models._greedy_selection``: value density order, weight capacity only.

    Items in ``decided`` keep their value; the others are taken in descending
    value/weight order (ties in item order) while the load fits the capacity.
    """
    items = _items(problem)
    decided = decided or {}
    order = sorted(range(len(items)), key=lambda i: items[i]["value"] / items[i]["weight"], reverse=True)
    selection = [decided.get(item["id"], False) for item in items]
    load = sum(item["weight"] for item, chosen in zip(items, selection, strict=True) if chosen)
    for index in order:
        if items[index]["id"] not in decided and load + items[index]["weight"] <= problem["capacity"]:
            selection[index] = True
            load += items[index]["weight"]
    return selection
