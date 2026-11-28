"""Container loading as a CP-SAT model with free placement.

The engine decides loading orders and derives positions from them with its decoder.
This model decides the positions themselves: for every item a container (or none), an
orientation and the corner (x, y, z), with

- every placed item inside its container,
- no two items of a container overlapping,
- at least `min_support_ratio` of every base on the floor or on the tops of other items
  (the contact areas are products of the overlap lengths, exactly as the engine counts them),

and the objective of the default constraint set: first the hard costs (items left out,
weight above a container's `max_weight`), then the soft ones (the cost of the containers
in use, groups spread over several containers), each at the default weight the engine's
catalog reports.

Every loading the engine can decode is a solution of this model, so on an instance it
covers the model's optimum is a bound no loading order can beat: an engine answer that
equals a proven optimum is optimal.

What it leaves out, and refuses: `max_load_on_top` (the load an item passes down is
shared by contact area, a division), `unload_order`, and the stacking rules, layer
limits, `bottom_only`, stack heights, balance limits, priorities, optional items and
group rules, allowed containers and segregation. It is quadratic in the number of
items (a product per pair for the contact areas), so it is for instances of up to
`MAX_ITEMS` items.
"""

from __future__ import annotations

import itertools
import math
from dataclasses import dataclass

from ortools.sat.python import cp_model

from optimaru import container_loading
from optimaru.container_loading.constraints import score
from optimaru.container_loading.models import ContainerLoadingProblemModel

from . import evaluator

#: The largest instance the model is built for.
MAX_ITEMS = 50
#: Container costs are counted in thousandths; a cost that is not a whole number of them is refused.
COST_SCALE = 1000


@dataclass(frozen=True)
class Answer:
    """What a solve returned: the solved model, and what CP-SAT says about it."""

    solved: ContainerLoadingProblemModel
    status: str  # "OPTIMAL" (proven) or "FEASIBLE"
    proven: bool


def _orientations(item) -> list[tuple[int, int, int]]:
    sides = (item.length, item.width, item.height)
    if item.rotations == "fixed":
        return [sides]
    if item.rotations == "upright":
        return sorted({sides, (item.width, item.length, item.height)})
    return sorted(set(itertools.permutations(sides)))


def _default_weights() -> dict[str, float]:
    """The engine's default weight per constraint; the model prices its costs with them."""
    return {row.name: row.default_weight for row in container_loading.constraint_info()}


def solve(problem: ContainerLoadingProblemModel, *, seconds: float, seed: int = 0, workers: int = 8) -> Answer:
    """Solve `problem` with free placement. Raises ValueError for an instance the model does not
    cover, and RuntimeError naming the CP-SAT status when it finds no solution in time or the
    instance is larger than `MAX_ITEMS`."""
    items, containers = problem.items, problem.containers
    if any(item.max_load_on_top is not None or item.unload_order is not None for item in items):
        raise ValueError("the CP-SAT model covers neither max_load_on_top nor unload_order")
    if (
        problem.stacking_rules
        or problem.group_rules
        or problem.segregation_rules
        or any(
            item.bottom_only
            or item.max_stack_layers is not None
            or item.stack_group
            or item.priority != 1
            or item.optional
            or item.allowed_containers
            or item.segregation_class is not None
            for item in items
        )
        or any(
            container.max_stack_height is not None
            or container.max_offset_length is not None
            or container.max_offset_width is not None
            for container in containers
        )
    ):
        raise ValueError(
            "the CP-SAT model covers no stacking rules, layer limits, stack heights, balance limits, "
            "priorities, optional items, group rules, allowed containers or segregation"
        )
    if len(items) > MAX_ITEMS:
        raise RuntimeError(f"the model is for up to {MAX_ITEMS} items, not {len(items)} (status=TOO_LARGE)")
    defaults = _default_weights()
    weights = {}
    for name in ("unplaced_item_cost", "container_weight_cost", "group_split_cost", "used_container_cost"):
        if defaults[name] != int(defaults[name]):
            raise ValueError(f"{name}: the default weight must be a whole number for CP-SAT")
        weights[name] = int(defaults[name])
    costs = [container.cost * COST_SCALE for container in containers]
    if any(abs(cost - round(cost)) > 1e-9 for cost in costs):
        raise ValueError(f"a container cost must be a whole number of 1/{COST_SCALE}")
    costs = [round(cost) for cost in costs]

    model = cp_model.CpModel()
    count, spaces = len(items), range(len(containers))
    far = [max(getattr(container, side) for container in containers) for side in ("length", "width", "height")]

    # Which container, which orientation, where.
    inside = [[model.new_bool_var(f"in[{i},{c}]") for c in spaces] for i in range(count)]
    left_out = [model.new_bool_var(f"out[{i}]") for i in range(count)]
    sizes = [_orientations(item) for item in items]
    turned = [[model.new_bool_var(f"o[{i},{k}]") for k in range(len(sizes[i]))] for i in range(count)]
    low = [[model.new_int_var(0, far[axis] - 1, f"p[{i},{axis}]") for axis in range(3)] for i in range(count)]
    high = [[model.new_int_var(1, far[axis], f"q[{i},{axis}]") for axis in range(3)] for i in range(count)]
    for i in range(count):
        model.add_exactly_one([*inside[i], left_out[i]])
        model.add_exactly_one(turned[i])
        for axis in range(3):
            extent = sum(turned[i][k] * sizes[i][k][axis] for k in range(len(sizes[i])))
            model.add(high[i][axis] == low[i][axis] + extent)
        for c in spaces:
            walls = (containers[c].length, containers[c].width, containers[c].height)
            for axis in range(3):
                model.add(high[i][axis] <= walls[axis]).only_enforce_if(inside[i][c])

    # Two items of one container do not overlap: one is beyond the other along some axis.
    together = {}
    for i, j in itertools.combinations(range(count), 2):
        same = model.new_bool_var(f"same[{i},{j}]")
        # same <=> both are placed, in one container.
        model.add_implication(same, ~left_out[i])
        model.add_implication(same, ~left_out[j])
        for c in spaces:
            model.add_bool_or([~inside[i][c], ~inside[j][c], same])
            model.add(inside[i][c] == inside[j][c]).only_enforce_if(same)
        apart = []
        for axis in range(3):
            for first, second in ((i, j), (j, i)):
                beyond = model.new_bool_var(f"apart[{first},{second},{axis}]")
                model.add(high[first][axis] <= low[second][axis]).only_enforce_if(beyond)
                apart.append(beyond)
        model.add_bool_or([*apart, ~same])
        together[i, j] = together[j, i] = same

    # Support. The length two items share along x and along y, and its product: the contact
    # area if one rests on the other.
    shared = {}
    for i, j in itertools.combinations(range(count), 2):
        lengths = []
        for axis in range(2):
            start = model.new_int_var(0, far[axis], f"start[{i},{j},{axis}]")
            end = model.new_int_var(0, far[axis], f"end[{i},{j},{axis}]")
            model.add_max_equality(start, [low[i][axis], low[j][axis]])
            model.add_min_equality(end, [high[i][axis], high[j][axis]])
            length = model.new_int_var(0, far[axis], f"shared[{i},{j},{axis}]")
            model.add_max_equality(length, [end - start, 0])
            lengths.append(length)
        area = model.new_int_var(0, far[0] * far[1], f"area[{i},{j}]")
        model.add_multiplication_equality(area, lengths)
        shared[i, j] = shared[j, i] = area
    for i in range(count):
        on_floor = model.new_bool_var(f"floor[{i}]")
        model.add(low[i][2] == 0).only_enforce_if(on_floor)
        carried = []
        for j in range(count):
            if j == i:
                continue
            rests = model.new_bool_var(f"rests[{i},{j}]")
            model.add_implication(rests, together[i, j])
            model.add(high[j][2] == low[i][2]).only_enforce_if(rests)
            contact = model.new_int_var(0, far[0] * far[1], f"contact[{i},{j}]")
            model.add(contact == shared[i, j]).only_enforce_if(rests)
            model.add(contact == 0).only_enforce_if(~rests)
            carried.append(contact)
        # The engine's rule with its tolerance: supported >= ratio * base, in whole units.
        needed = sum(
            turned[i][k] * math.ceil(problem.min_support_ratio * size[0] * size[1] - 1e-6)
            for k, size in enumerate(sizes[i])
        )
        model.add(sum(carried) >= needed).only_enforce_if([~on_floor, ~left_out[i]])

    # The costs.
    hard = [weights["unplaced_item_cost"] * out for out in left_out]
    used = [model.new_bool_var(f"used[{c}]") for c in spaces]
    for c in spaces:
        container = containers[c]
        for i in range(count):
            model.add_implication(inside[i][c], used[c])
        volume = container.length * container.width * container.height
        model.add(sum(inside[i][c] * sizes[i][0][0] * sizes[i][0][1] * sizes[i][0][2] for i in range(count)) <= volume)
        if container.max_weight is not None:
            load = sum(inside[i][c] * items[i].weight for i in range(count))
            excess = model.new_int_var(0, sum(item.weight for item in items), f"excess[{c}]")
            model.add(excess >= load - container.max_weight)
            hard.append(weights["container_weight_cost"] * excess)
    soft = [weights["used_container_cost"] * costs[c] * used[c] for c in spaces]
    groups: dict[str, list[int]] = {}
    for i, item in enumerate(items):
        if item.group is not None:
            groups.setdefault(item.group, []).append(i)
    for name, members in groups.items():
        holds = [model.new_bool_var(f"holds[{name},{c}]") for c in spaces]
        for c in spaces:
            for i in members:
                model.add_implication(inside[i][c], holds[c])
        split = model.new_int_var(0, len(containers), f"split[{name}]")
        model.add(split >= sum(holds) - 1)
        soft.append(weights["group_split_cost"] * COST_SCALE * split)
    # Containers that are alike are used in their order: one optimum instead of many.
    for c in spaces[1:]:
        before, here = containers[c - 1], containers[c]
        if (before.length, before.width, before.height, before.cost, before.max_weight) == (
            here.length,
            here.width,
            here.height,
            here.cost,
            here.max_weight,
        ):
            model.add_implication(used[c], used[c - 1])
    # Hard before soft: one unit of hard cost outweighs every soft cost there can be.
    most_soft = (
        weights["used_container_cost"] * sum(costs)
        + weights["group_split_cost"] * COST_SCALE * len(groups) * len(containers)
        + 1
    )
    model.minimize(most_soft * sum(hard) + sum(soft))

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = float(seconds)
    solver.parameters.num_workers = int(workers)
    solver.parameters.random_seed = int(seed)
    status = solver.solve(model)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(f"CP-SAT found no loading (status={solver.status_name(status)})")

    placed = {
        i: (
            c,
            tuple(solver.value(low[i][axis]) for axis in range(3)),
            tuple(solver.value(high[i][axis]) for axis in range(3)),
        )
        for i in range(count)
        for c in spaces
        if solver.value(inside[i][c])
    }
    return Answer(_solved(problem, placed), solver.status_name(status), status == cp_model.OPTIMAL)


def _solved(problem: ContainerLoadingProblemModel, placed: dict) -> ContainerLoadingProblemModel:
    """`problem` with the positions CP-SAT chose written as the outputs of a solve."""
    solved = problem.model_copy(deep=True)
    for c, container in enumerate(solved.containers):
        here = sorted(
            (i for i in placed if placed[i][0] == c), key=lambda i: (placed[i][1][0], placed[i][1][2], placed[i][1][1])
        )
        for sequence, i in enumerate(here):
            item, (_, corner, far) = solved.items[i], placed[i]
            item.container, item.sequence = container.id, sequence
            item.x, item.y, item.z = corner
            item.placed_length, item.placed_width, item.placed_height = (far[axis] - corner[axis] for axis in range(3))
        boxes = [solved.items[i] for i in here]
        container.items = [box.id for box in boxes]
        container.unplaced_items = []
        container.loaded_weight = sum(box.weight for box in boxes)
        container.loaded_volume = sum(box.length * box.width * box.height for box in boxes)
        container.fill_ratio = container.loaded_volume / (container.length * container.width * container.height)
        container.used_length = max((box.x + box.placed_length for box in boxes), default=0)
    solved.containers_used = sum(1 for container in solved.containers if container.items)
    solved.unplaced_items = [item.id for i, item in enumerate(solved.items) if i not in placed]
    result = score(evaluator.constraint_costs(solved))
    solved.score_hard, solved.score_soft = result.hard, result.soft
    return solved
