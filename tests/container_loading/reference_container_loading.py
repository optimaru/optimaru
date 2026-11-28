"""An independent check of a solved container-loading model, in plain Python.

It does not decode anything. It takes the positions a solve returned and

- `loading_faults(model)` checks that they are a loading at all: every placed item inside
  its container and below its `max_stack_height`, in an orientation its `rotations` allow,
  overlapping no other item, with at least `min_support_ratio` of its base on the floor or on
  the tops of other items, on the floor when `bottom_only`, and the
  per-container outputs (`items`, `sequence`, `loaded_weight`, `loaded_volume`, `fill_ratio`,
  `used_length`) agreeing with the items;
- `raw_costs(model)` recomputes every constraint's raw cost from those positions, written
  from the constraints' definitions (docs/use-cases/container-loading/README.md) and not from
  the C++: the stacking loads with exact fractions, the rest with integers.

The tests compare both with what the engine reports (`test_reference_parity.py`).
"""

from __future__ import annotations

import itertools
import math
from fractions import Fraction


def _span(low_a, size_a, low_b, size_b):
    """Length two intervals share."""
    return max(0, min(low_a + size_a, low_b + size_b) - max(low_a, low_b))


def _placed(model, container):
    """The container's placed items, in loading order."""
    boxes = [item for item in model.items if item.container == container.id]
    return sorted(boxes, key=lambda item: item.sequence)


def _allowed_sizes(item):
    sides = (item.length, item.width, item.height)
    if item.rotations == "fixed":
        return {sides}
    if item.rotations == "upright":
        return {sides, (item.width, item.length, item.height)}
    return set(itertools.permutations(sides))


def loading_faults(model) -> list[str]:
    """What is wrong with the positions of `model` (a solved problem model); [] = a valid loading."""
    faults = []
    listed = set()
    for container in model.containers:
        boxes = _placed(model, container)
        if [box.id for box in boxes] != container.items:
            faults.append(f"{container.id}: items is not the placed items in sequence order")
        if [box.sequence for box in boxes] != list(range(len(boxes))):
            faults.append(f"{container.id}: sequences are not 0..n-1")
        listed.update(container.items)
        listed.update(container.unplaced_items)
        names = {container.id, *container.tags}
        for box in boxes:
            if box.allowed_containers and not names & set(box.allowed_containers):
                faults.append(f"{box.id}: in {container.id}, which its allowed_containers leave out")
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
            if container.max_stack_height is not None and box.z + box.placed_height > container.max_stack_height:
                faults.append(f"{box.id}: above the stack height of {container.id}")
            if box.bottom_only and box.z > 0:
                faults.append(f"{box.id}: bottom_only, yet off the floor")
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
        weight = sum(box.weight for box in boxes)
        volume = sum(box.length * box.width * box.height for box in boxes)
        capacity = container.length * container.width * container.height
        used_length = max((box.x + box.placed_length for box in boxes), default=0)
        if (container.loaded_weight, container.loaded_volume, container.used_length) != (weight, volume, used_length):
            faults.append(f"{container.id}: loaded_weight, loaded_volume or used_length differ from its items")
        if not math.isclose(container.fill_ratio, volume / capacity):
            faults.append(f"{container.id}: fill_ratio is not loaded_volume / volume")
    unplaced = [item.id for item in model.items if item.container is None]
    if unplaced != model.unplaced_items:
        faults.append("unplaced_items is not the items without a container")
    for item in model.items:
        if item.container is None and any(
            value is not None for value in (item.sequence, item.x, item.y, item.z, item.placed_length)
        ):
            faults.append(f"{item.id}: not placed, yet it has a position")
    if model.containers_used != sum(1 for container in model.containers if container.items):
        faults.append("containers_used is not the containers with a placed item")
    return faults


def _stacking_excess(boxes) -> int:
    """Weight resting on each item above its max_load_on_top, in whole units, summed."""
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
        in_front = b.x >= a.x + a.placed_length and over_y and over_z
        on_top = b.z >= a.z + a.placed_height and over_x and over_y
        blocks += in_front or on_top
    return blocks


def _stacking_violations(model, boxes) -> int:
    """Pairs where an item stands on another against a stacking rule, plus the layers above each
    item's max_stack_layers in the tallest column through it."""
    forbidden = {(rule.top, rule.bottom) for rule in model.stacking_rules}
    under = {box.id: [] for box in boxes}
    for box, other in itertools.permutations(boxes, 2):
        touching = _span(box.x, box.placed_length, other.x, other.placed_length) and _span(
            box.y, box.placed_width, other.y, other.placed_width
        )
        if other.z + other.placed_height == box.z and touching:
            under[box.id].append(other)
    pairs = sum((box.stack_group, other.stack_group) in forbidden for box in boxes for other in under[box.id])
    level, above = {}, {box.id: 0 for box in boxes}
    for box in sorted(boxes, key=lambda box: box.z):  # bottom up
        level[box.id] = 1 + max((level[other.id] for other in under[box.id]), default=0)
    for box in sorted(boxes, key=lambda box: -box.z):  # top down
        for other in under[box.id]:
            above[other.id] = max(above[other.id], above[box.id] + 1)
    layers = sum(
        max(0, level[box.id] + above[box.id] - box.max_stack_layers)
        for box in boxes
        if box.max_stack_layers is not None
    )
    return pairs + layers


def _balance_excess(container, boxes) -> int:
    """Percent of the half size the centre of gravity lies beyond each limit, rounded up; a
    millionth of a percent over is not counted (a limit such as 0.3 is not exact in binary)."""
    weight = sum(box.weight for box in boxes)
    if weight == 0:
        return 0
    excess = 0
    for limit, size, centre in (
        (container.max_offset_length, container.length, lambda box: 2 * box.x + box.placed_length),
        (container.max_offset_width, container.width, lambda box: 2 * box.y + box.placed_width),
    ):
        if limit is None:
            continue
        # The centre of gravity times 2, as an exact fraction; the offset as a share of half the size.
        doubled = Fraction(sum(box.weight * centre(box) for box in boxes), weight)
        over = 100 * (abs(doubled - size) / size - Fraction(limit))
        tolerance = Fraction(1, 10**6)
        if over > tolerance:
            excess += math.ceil(over - tolerance)
    return excess


def _segregation_pairs(model, boxes) -> int:
    """Pairs kept out of one container by a rule, plus pairs closer than a rule's distance (the
    larger of the gaps between their footprints along x and along y)."""
    keep = {}
    for rule in model.segregation_rules:
        wanted = -1 if rule.distance is None else rule.distance
        for pair in ((rule.a, rule.b), (rule.b, rule.a)):
            old = keep.get(pair, 0)
            keep[pair] = -1 if -1 in (old, wanted) else max(old, wanted)
    pairs = 0
    for a, b in itertools.combinations(boxes, 2):
        need = keep.get((a.segregation_class, b.segregation_class), 0)
        gap_x = max(0, b.x - (a.x + a.placed_length), a.x - (b.x + b.placed_length))
        gap_y = max(0, b.y - (a.y + a.placed_width), a.y - (b.y + b.placed_width))
        pairs += need == -1 or (need > 0 and max(gap_x, gap_y) < need)
    return pairs


def _balance(container, boxes) -> int:
    weight = sum(box.weight for box in boxes)
    if weight == 0:
        return 0
    centre_x = sum(box.weight * (box.x + box.placed_length / 2) for box in boxes) / weight
    centre_y = sum(box.weight * (box.y + box.placed_width / 2) for box in boxes) / weight
    # sqrt as the engine takes it (not hypot, which may differ in the last bit and round a
    # value of exactly n + 0.5 thousandths the other way).
    dx, dy = centre_x - container.length / 2, centre_y - container.width / 2
    half_x, half_y = container.length / 2, container.width / 2
    share = math.sqrt(dx * dx + dy * dy) / math.sqrt(half_x * half_x + half_y * half_y)
    return math.floor(1000 * share + 0.5)


def raw_costs(model) -> dict[str, list[float]]:
    """Every constraint's raw cost on a solved model: one per container, or the one total of
    group_split_cost. The keys and the shape are `constraint_costs()`'s."""
    by_id = {item.id: item for item in model.items}
    costs: dict[str, list[float]] = {
        name: []
        for name in (
            "unplaced_item_cost",
            "container_weight_cost",
            "stacking_load_cost",
            "stacking_rule_cost",
            "unloading_order_cost",
            "balance_limit_cost",
            "segregation_cost",
            "used_container_cost",
            "optional_item_cost",
            "used_length_cost",
            "load_balance_cost",
        )
    }
    containers_of_group: dict[str, set[str]] = {}
    for container in model.containers:
        boxes = _placed(model, container)
        given = [*container.items, *container.unplaced_items]
        left_out = [by_id[item_id] for item_id in container.unplaced_items]
        # An unplaced optional item is out of the load: its weight does not count.
        weight = sum(by_id[item_id].weight for item_id in given) - sum(i.weight for i in left_out if i.optional)
        limit = container.max_weight
        costs["unplaced_item_cost"].append(sum(item.priority for item in left_out if not item.optional))
        costs["optional_item_cost"].append(sum(item.priority for item in left_out if item.optional))
        costs["container_weight_cost"].append(max(0, weight - limit) if limit is not None else 0)
        costs["stacking_load_cost"].append(_stacking_excess(boxes))
        costs["stacking_rule_cost"].append(_stacking_violations(model, boxes))
        costs["unloading_order_cost"].append(_unloading_blocks(boxes))
        costs["balance_limit_cost"].append(_balance_excess(container, boxes))
        costs["segregation_cost"].append(_segregation_pairs(model, boxes))
        costs["used_container_cost"].append(container.cost if boxes else 0.0)
        costs["used_length_cost"].append(max((box.x + box.placed_length for box in boxes), default=0))
        costs["load_balance_cost"].append(_balance(container, boxes))
        for item_id in given:
            if by_id[item_id].group is not None:
                containers_of_group.setdefault(by_id[item_id].group, set()).add(container.id)
    costs["group_split_cost"] = [sum(len(places) - 1 for places in containers_of_group.values())]
    incomplete = single = 0
    for rule in model.group_rules:
        members = [item for item in model.items if item.group == rule.id]
        listed = [item for item in members if any(item.id in [*c.items, *c.unplaced_items] for c in model.containers)]
        placed = [item for item in listed if item.container is not None]
        if rule.all_or_none:
            incomplete += min(len(placed), len(listed) - len(placed))
        if rule.single_container:
            single += max(0, len({item.container for item in placed}) - 1)
    costs["incomplete_group_cost"] = [incomplete]
    costs["single_container_cost"] = [single]
    return costs
