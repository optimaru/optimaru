"""The OR-Tools CP-SAT model of box sizing: the exact reference the engine is compared with.

Parcels that fit the same candidate boxes (by size and by weight) pay the same for a box
up to a constant, so in an optimum they ship together: they form one group. The model has
one boolean per (group, box) pair that fits and one per box that says the box is in use::

    every group ships in exactly one box that takes it
    a group ships only in a box in use; a required box is in use
    at most max_box_types boxes are in use
    minimize  sum over the pairs chosen of
              sum over the group's parcels of
              quantity * (box volume - parcel volume) + quantity * box cost

Grouping loses nothing: the cost of a group in a box is the sum of its parcels' costs
there, and a solution that splits a group can move all of it to the cheaper of the boxes
it uses without using another box.

Two opt-in costs of the use case can be added to the objective with their weights
(`board_area_weight`, `shipping_weight`, both 0 by default, as the engine's
`board_area_cost` and `shipping_cost` are): quantity x the box's board 2 (lw + lh + wh), and
quantity x the price of the billed weight (max(parcel weight, ceil(box volume /
dim_divisor))) in the rate table of the parcel's zone. A shipping price depends on the
parcel, so with a shipping weight the groups are the parcels that fit the same boxes and
pay the same price in each of them.

The three hard constraints of the use case (`parcel_fit_cost`, `box_weight_cost`,
`box_type_limit_cost`) are structural here: a pair that does not fit has no variable, and
the limit is a linear constraint, so a solution of the model has hard 0. The objective is
`void_volume_cost` + `box_cost` at their default weights (1 each); `box_type_cost`, off by
default, is not modelled. A box's `cost` (and a weighted board area or shipping price) must
be a multiple of 0.001: CP-SAT's objective is integral, and the costs are scaled by the
smallest power of ten that makes them whole.

`solve` takes and returns the use case's own model
(`optimaru.box_sizing.models.BoxSizingProblemModel`), so its answer is rescored by the
engine's evaluator.
"""

from __future__ import annotations

from ortools.sat.python import cp_model

from optimaru.box_sizing.models import BoxSizingProblemModel

__all__ = ["billed_weight", "board_area", "build", "cost_scale", "groups", "shipping_price", "solve"]

#: A model whose scaled objective could pass this is refused: CP-SAT's objective is an int64.
MAX_OBJECTIVE = 2**62


def _fits(parcel, box) -> bool:
    sides = zip(sorted((parcel.length, parcel.width, parcel.height)), sorted((box.length, box.width, box.height)))
    return all(p <= b for p, b in sides) and (box.max_weight is None or parcel.weight <= box.max_weight)


def board_area(box) -> int:
    """The board of a box: 2 (lw + lh + wh)."""
    return 2 * (box.length * box.width + box.length * box.height + box.width * box.height)


def billed_weight(parcel, box) -> int:
    """The parcel's weight, or the box's dimensional weight (ceil(volume / dim_divisor)) when
    that is more."""
    if box.dim_divisor is None:
        return parcel.weight
    return max(parcel.weight, -(-(box.length * box.width * box.height) // box.dim_divisor))


def shipping_price(problem: BoxSizingProblemModel, parcel, box) -> float:
    """What one shipment of the parcel costs to ship in the box; 0 without rate tables."""
    if not problem.rate_tables:
        return 0.0
    table = next(table for table in problem.rate_tables if table.zone == parcel.zone)
    billed = billed_weight(parcel, box)
    for rate_break in table.breaks:
        if billed <= rate_break.max_weight:
            return rate_break.price
    raise ValueError(f"parcel {parcel.id!r} bills {billed} in box {box.id!r}, above its rate table")


def cost_scale(problem: BoxSizingProblemModel, *, board_area_weight: float = 0.0, shipping_weight: float = 0.0) -> int:
    """The smallest of 1, 10, 100, 1000 that makes every box cost a whole number, and with
    the weights, every weighted board area and every weighted rate-table price."""
    values = [box.cost for box in problem.boxes]
    if board_area_weight:
        values += [board_area_weight * board_area(box) for box in problem.boxes]
    if shipping_weight:
        values += [shipping_weight * b.price for table in problem.rate_tables for b in table.breaks]
    for scale in (1, 10, 100, 1000):
        if all(abs(value * scale - round(value * scale)) < 1e-9 * max(1.0, abs(value)) for value in values):
            return scale
    raise ValueError(
        "the CP-SAT model takes box costs (and weighted board areas and prices) that are multiples of 0.001"
    )


def groups(problem: BoxSizingProblemModel, *, by_price: bool = False) -> dict[tuple, list[int]]:
    """The parcels (by index) that fit the same boxes, keyed by those boxes' indices. With
    `by_price`, also by the shipping price in each of them: the key is (boxes, prices)."""
    grouped: dict[tuple, list[int]] = {}
    for p, parcel in enumerate(problem.parcels):
        fitting = tuple(b for b, box in enumerate(problem.boxes) if _fits(parcel, box))
        if not fitting:
            raise ValueError(f"parcel {parcel.id!r} fits none of the candidate boxes")
        key = fitting
        if by_price:
            key = (fitting, tuple(shipping_price(problem, parcel, problem.boxes[b]) for b in fitting))
        grouped.setdefault(key, []).append(p)
    return grouped


def build(problem: BoxSizingProblemModel, *, board_area_weight: float = 0.0, shipping_weight: float = 0.0):
    """The model, its variables as ``[(parcel indices, box index, variable)]`` and the cost scale.

    The objective is the void volume and the box cost, plus `board_area_weight` x the board
    area and `shipping_weight` x the shipping cost: the soft score of the engine's default
    constraints with `board_area_cost` and `shipping_cost` at those weights."""
    scale = cost_scale(problem, board_area_weight=board_area_weight, shipping_weight=shipping_weight)
    model = cp_model.CpModel()
    in_use = [model.new_bool_var(f"use_{box.id}") for box in problem.boxes]
    volume = [box.length * box.width * box.height for box in problem.boxes]
    # Per shipment, scaled: the box's cost and its weighted board.
    cost = [round(box.cost * scale) + round(board_area_weight * board_area(box) * scale) for box in problem.boxes]
    by_price = bool(shipping_weight) and bool(problem.rate_tables)
    ships = []
    objective = []
    bound = 0
    for g, (key, members) in enumerate(groups(problem, by_price=by_price).items()):
        fitting = key[0] if by_price else key
        quantity = sum(problem.parcels[p].quantity for p in members)
        shipped = sum(
            problem.parcels[p].quantity
            * problem.parcels[p].length
            * problem.parcels[p].width
            * problem.parcels[p].height
            for p in members
        )
        options = []
        dearest = 0
        for b in fitting:
            variable = model.new_bool_var(f"ship_{g}_{b}")
            ships.append((members, b, variable))
            options.append(variable)
            model.add_implication(variable, in_use[b])
            price = scale * (quantity * volume[b] - shipped) + quantity * cost[b]
            if by_price:
                unit = shipping_price(problem, problem.parcels[members[0]], problem.boxes[b])
                price += quantity * round(shipping_weight * unit * scale)
            objective.append(price * variable)
            dearest = max(dearest, price)
        model.add_exactly_one(options)
        bound += dearest
    if bound >= MAX_OBJECTIVE:
        raise ValueError("the objective does not fit CP-SAT's int64: use a coarser length unit")
    for b, box in enumerate(problem.boxes):
        if box.required:
            model.add(in_use[b] == 1)
    if problem.max_box_types is not None:
        model.add(sum(in_use) <= problem.max_box_types)
    model.minimize(sum(objective))
    return model, ships, scale


def solve(
    problem: BoxSizingProblemModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    parallel_solver_count: int = 8,
    board_area_weight: float = 0.0,
    shipping_weight: float = 0.0,
) -> BoxSizingProblemModel:
    """Solve `problem` with CP-SAT and return it solved, rescored by the engine.

    `board_area_weight` and `shipping_weight` add the two opt-in costs to the objective (see
    `build`). The returned model's score is still the engine's default one: score it with
    the picks of those weights to compare.

    The returned model is ``BoxSizingProblemModel.from_core`` of the engine's problem with
    CP-SAT's assignment, so its score and outputs are the engine's own. Raises RuntimeError
    naming the status when the search ends without a solution.
    """
    import optimaru.box_sizing as bs

    model, ships, _ = build(problem, board_area_weight=board_area_weight, shipping_weight=shipping_weight)
    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = float(time_limit_seconds)
    solver.parameters.random_seed = int(random_seed)
    solver.parameters.num_workers = int(parallel_solver_count)
    status = solver.solve(model)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(f"CP-SAT found no solution (status={solver.status_name(status)})")
    core = problem.to_core()
    assignment = [-1] * len(problem.parcels)
    for members, b, variable in ships:
        if solver.boolean_value(variable):
            for p in members:
                assignment[p] = b
    core.set_assignment(assignment)
    bs.calculate_score(core)
    return BoxSizingProblemModel.from_core(core)
