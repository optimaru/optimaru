"""Scores a loading from where its items stand, for both solvers of the comparison.

The engine derives positions from loading orders with its decoder; the CP-SAT model
chooses positions freely. To compare them on equal terms, neither answer is scored by
the engine's decoder: both are turned into a solved `ContainerLoadingProblemModel` and
scored here from the positions alone, by the constraints' definitions
(docs/use-cases/container-loading/README.md), after a check that the positions are a
loading at all (inside the container, no overlap, enough support, an allowed
orientation). An answer that is not raises ValueError: a bug in a solver, not a score.

This module has the shape the comparison runner asks of a use case package
(`constraint_costs`, and `constraints.score` beside it), so the benchmark names it as
its `PACKAGE`. For the engine's answers the runner also compares the score computed
here with the one the engine reported itself.

Two of the constraints are outside the CP-SAT model (stacking loads and unloading
order); they are scored here all the same, so an instance that uses them is still
compared correctly for the engine and refused by the model. The stacking rules, layer
limits, `bottom_only`, stack heights, balance limits, priorities, optional items and group
rules are not scored here: no instance
of the comparison sets them, and a model that does is refused (ValueError).
"""

from __future__ import annotations

import itertools
import math
from fractions import Fraction
from typing import Any

from optimaru import container_loading
from optimaru.container_loading.models import ContainerLoadingProblemModel

__all__ = ["as_model", "constraint_costs", "loading_faults", "raw_costs"]


def as_model(solution: Any) -> ContainerLoadingProblemModel:
    """A solved model of `solution`: the engine's problem (decoded and scored by the engine), or
    a model that already carries positions (the CP-SAT answer)."""
    if isinstance(solution, ContainerLoadingProblemModel):
        return solution
    return ContainerLoadingProblemModel.from_core(solution)


def _span(low_a: int, size_a: int, low_b: int, size_b: int) -> int:
    return max(0, min(low_a + size_a, low_b + size_b) - max(low_a, low_b))


def _placed(model, container) -> list:
    boxes = [item for item in model.items if item.container == container.id]
    return sorted(boxes, key=lambda item: item.sequence)


def _allowed_sizes(item) -> set[tuple[int, int, int]]:
    sides = (item.length, item.width, item.height)
    if item.rotations == "fixed":
        return {sides}
    if item.rotations == "upright":
        return {sides, (item.width, item.length, item.height)}
    return set(itertools.permutations(sides))


def loading_faults(model: ContainerLoadingProblemModel) -> list[str]:
    """What is wrong with the positions of a solved model; [] = a valid loading."""
    faults = []
    for container in model.containers:
        boxes = _placed(model, container)
        if [box.id for box in boxes] != container.items:
            faults.append(f"{container.id}: items is not the placed items in sequence order")
        for box in boxes:
            size = (box.placed_length, box.placed_width, box.placed_height)
            if size not in _allowed_sizes(box):
                faults.append(f"{box.id}: placed as {size}, which rotations={box.rotations} does not allow")
            if (
                min(box.x, box.y, box.z) < 0
                or box.x + box.placed_length > container.length
                or box.y + box.placed_width > container.width
                or box.z + box.placed_height > container.height
            ):
                faults.append(f"{box.id}: outside {container.id}")
            supported = 0
            for other in boxes:
                if other is box:
                    continue
                dx = _span(box.x, box.placed_length, other.x, other.placed_length)
                dy = _span(box.y, box.placed_width, other.y, other.placed_width)
                dz = _span(box.z, box.placed_height, other.z, other.placed_height)
                if dx and dy and dz:
                    faults.append(f"{box.id} overlaps {other.id}")
                if other.z + other.placed_height == box.z:
                    supported += dx * dy
            base = box.placed_length * box.placed_width
            if box.z > 0 and supported < model.min_support_ratio * base - 1e-6:
                faults.append(f"{box.id}: only {supported} of its base of {base} is supported")
    return faults


def _stacking_excess(boxes) -> int:
    load = {box.id: Fraction(0) for box in boxes}
    for box in sorted(boxes, key=lambda box: -box.z):  # top down
        if box.z == 0:
            continue
        below = []
        for other in boxes:
            if other.z + other.placed_height != box.z:
                continue
            area = _span(box.x, box.placed_length, other.x, other.placed_length) * _span(
                box.y, box.placed_width, other.y, other.placed_width
            )
            if area:
                below.append((other, area))
        contact = sum(area for _, area in below)
        for other, area in below:
            load[other.id] += (box.weight + load[box.id]) * Fraction(area, contact)
    return sum(
        math.ceil(load[box.id] - box.max_load_on_top)
        for box in boxes
        if box.max_load_on_top is not None and load[box.id] > box.max_load_on_top
    )


def _unloading_blocks(boxes) -> int:
    blocks = 0
    for a, b in itertools.permutations(boxes, 2):
        if a.unload_order is None or b.unload_order is None or not a.unload_order < b.unload_order:
            continue
        over_x = _span(a.x, a.placed_length, b.x, b.placed_length) > 0
        over_y = _span(a.y, a.placed_width, b.y, b.placed_width) > 0
        over_z = _span(a.z, a.placed_height, b.z, b.placed_height) > 0
        blocks += (b.x >= a.x + a.placed_length and over_y and over_z) or (
            b.z >= a.z + a.placed_height and over_x and over_y
        )
    return blocks


def _balance(container, boxes) -> int:
    weight = sum(box.weight for box in boxes)
    if weight == 0:
        return 0
    centre_x = sum(box.weight * (box.x + box.placed_length / 2) for box in boxes) / weight
    centre_y = sum(box.weight * (box.y + box.placed_width / 2) for box in boxes) / weight
    offset = math.hypot(centre_x - container.length / 2, centre_y - container.width / 2)
    return math.floor(1000 * offset / math.hypot(container.length / 2, container.width / 2) + 0.5)


def _refuse_unscored_inputs(model: ContainerLoadingProblemModel) -> None:
    """The inputs this evaluator does not score: a model that sets one is refused."""
    unscored = [
        f"item {item.id}"
        for item in model.items
        if item.bottom_only
        or item.max_stack_layers is not None
        or item.stack_group is not None
        or item.priority != 1
        or item.optional
    ] + [
        f"container {container.id}"
        for container in model.containers
        if container.max_stack_height is not None
        or container.max_offset_length is not None
        or container.max_offset_width is not None
    ]
    if model.stacking_rules:
        unscored.append("the problem's stacking_rules")
    if model.group_rules:
        unscored.append("the problem's group_rules")
    if unscored:
        raise ValueError(
            "the evaluator does not score stacking rules, layers, balance limits, priorities, optional "
            f"items or group rules: {unscored[:3]}"
        )


def raw_costs(model: ContainerLoadingProblemModel) -> dict[str, list[float]]:
    """Every constraint's raw cost from the positions of a solved model: one per container, or
    the one total of group_split_cost."""
    _refuse_unscored_inputs(model)
    by_id = {item.id: item for item in model.items}
    names = [name for name in container_loading.list_constraints() if name != "group_split_cost"]
    costs: dict[str, list[float]] = {name: [] for name in names}
    containers_of_group: dict[str, set[str]] = {}
    for container in model.containers:
        boxes = _placed(model, container)
        given = [*(container.items or []), *(container.unplaced_items or [])]
        weight = sum(by_id[item_id].weight for item_id in given)
        limit = container.max_weight
        costs["unplaced_item_cost"].append(len(container.unplaced_items or []))
        costs["container_weight_cost"].append(max(0, weight - limit) if limit is not None else 0)
        costs["stacking_load_cost"].append(_stacking_excess(boxes))
        costs["stacking_rule_cost"].append(0)  # refused above when anything could break one
        costs["unloading_order_cost"].append(_unloading_blocks(boxes))
        costs["balance_limit_cost"].append(0)
        costs["optional_item_cost"].append(0)
        costs["used_container_cost"].append(container.cost if boxes else 0.0)
        costs["used_length_cost"].append(max((box.x + box.placed_length for box in boxes), default=0))
        costs["load_balance_cost"].append(_balance(container, boxes))
        for item_id in given:
            if by_id[item_id].group is not None:
                containers_of_group.setdefault(by_id[item_id].group, set()).add(container.id)
    costs["group_split_cost"] = [sum(len(places) - 1 for places in containers_of_group.values())]
    costs["incomplete_group_cost"] = [0]  # refused above when a group rule is set
    costs["single_container_cost"] = [0]
    return costs


def constraint_costs(solution: Any) -> dict[str, dict[str, Any]]:
    """`optimaru.container_loading.constraint_costs` in shape, computed from the positions.

    The knobs of every entry (default weight and level) are the engine's own, read off its
    catalog on the same problem, so `constraints.score` prices both solvers alike. Raises
    ValueError when the positions are not a valid loading."""
    model = as_model(solution)
    faults = loading_faults(model)
    if faults:
        raise ValueError("not a valid loading: " + "; ".join(faults[:5]))
    costs = raw_costs(model)
    # An item no container holds (the CP-SAT model leaves one out instead of giving it to a
    # container) is an unplaced item all the same: charged to the first container.
    listed = {item_id for c in model.containers for item_id in [*(c.items or []), *(c.unplaced_items or [])]}
    costs["unplaced_item_cost"][0] += sum(1 for item in model.items if item.id not in listed)
    catalog = container_loading.constraint_costs(model.to_core())
    return {name: {**entry, "costs": costs[name]} for name, entry in catalog.items()}
