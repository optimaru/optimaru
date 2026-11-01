"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`op.solve(solution)`) enables all four constraints at their default
weights. This module is the alternative path next to it: pick constraints, change weights,
flip hard/soft, shape the cost, and pass the list to `solve(solution, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import order_picking as op
    from order_picking.constraints import (
        DistanceFromLastPickTaskToPathOriginCost, DistanceFromPreviousPickTaskCost,
        OrderSplitByTrolleyCost, TrolleyCapacityCost, get_constraints,
    )

    op.solve(solution, constraints=get_constraints())           # the defaults as-is
    op.solve(solution, constraints=[                            # picked
        TrolleyCapacityCost(), OrderSplitByTrolleyCost(weight=100),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``trolley_capacity_cost``
is ``TrolleyCapacityCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (OrderSplitByTrolleyCost 1000, the other three 1);
               ``op.constraint_info()`` reports the defaults.
- ``hard``     ``True`` = hard, ``False`` = soft, ``None`` = the default level.
- ``exponent`` sign-preserving power shaping per trolley: ``weight * sign(c) *
               |c| ** exponent``. 2.0 = squared, so concentrated violations grow
               much costlier.
- ``offset``   (keyword) shifts that shaping per trolley; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a trolley below the offset — an empty
               one (cost 0) included — lowers the penalty.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs and the picks add up. The walk plus the square of every meter beyond
100 m per trolley::

    op.solve(solution, constraints=[
        TrolleyCapacityCost(),
        DistanceFromPreviousPickTaskCost(),
        DistanceFromPreviousPickTaskCost(weight=0.1, exponent=2, offset=100),
        DistanceFromLastPickTaskToPathOriginCost(),
        OrderSplitByTrolleyCost(),
    ])

``op.constraint_costs(solution, picks)`` keys the second pick
``"distance_from_previous_pick_task_cost#2"``.

Names (`name`) are the C++ component names in snake_case
(``TrolleyCapacityCost`` -> ``trolley_capacity_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`op.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.

Two knobs do not apply on the plugin path: `parallel_solver_count`
(a caller-owned calculator cannot be cloned into workers) and `order_consolidate_moves`
(that operator reads the monolithic calculator's live volume table). Details are in
the comments of `cpp/include/op/solver.hpp`.
"""

from ._constraint import Constraint, Score, score_costs, select

__all__ = [
    "Constraint",
    "Score",
    "score",
    "TrolleyCapacityCost",
    "DistanceFromPreviousPickTaskCost",
    "DistanceFromLastPickTaskToPathOriginCost",
    "OrderSplitByTrolleyCost",
    "trolley_capacity_cost",
    "distance_from_previous_pick_task_cost",
    "distance_from_last_pick_task_to_path_origin_cost",
    "order_split_by_trolley_cost",
    "get_constraints",
]


class TrolleyCapacityCost(Constraint):
    """Buckets a trolley is short of. Default: hard, weight 1.

    Orders never share buckets, so bucket counts round up the volume of **each
    order** on the trolley — that rounding is exactly why splitting an order over
    several trolleys wastes capacity."""

    name = "trolley_capacity_cost"
    label_en = "Trolley bucket capacity"
    label_ko = "트롤리 버킷 용량"
    description_en = (
        "Buckets a trolley needs beyond the buckets it carries. Each order on the trolley "
        "rounds its own volume up to whole buckets (orders never share a bucket). One unit "
        "of cost is one missing bucket on one trolley; 0 means every trolley has enough buckets."
    )
    description_ko = (
        "트롤리에 필요한 버킷 수가 실제로 실은 버킷 수를 넘는 만큼을 더합니다. 주문끼리는 버킷을 "
        "나눠 쓰지 않으므로 트롤리 위의 주문마다 부피를 버킷 단위로 올림해 셉니다. 모자란 버킷 "
        "1개가 비용 1이며, 0이면 모든 트롤리의 버킷이 충분합니다."
    )


class DistanceFromPreviousPickTaskCost(Constraint):
    """Travel distance from each pick to the next. Default: soft, weight 1.

    For the first pick, the trolley's parking spot stands in as the "previous location"."""

    name = "distance_from_previous_pick_task_cost"
    label_en = "Distance between picks"
    label_ko = "피킹 지점 간 이동 거리"
    description_en = (
        "The distance a trolley travels from its start location to its first pick and from "
        "each pick to the next, in meters (the way back is a separate constraint). One unit "
        "of cost is one meter; 0 means no trolley has to travel to its picks."
    )
    description_ko = (
        "트롤리가 출발 위치에서 첫 피킹 지점까지, 그리고 피킹 지점에서 다음 피킹 지점까지 이동하는 "
        "거리(미터)를 더합니다(돌아오는 길은 별도 제약입니다). 1미터가 비용 1이며, 0이면 어떤 "
        "트롤리도 피킹하러 이동할 필요가 없습니다."
    )


class DistanceFromLastPickTaskToPathOriginCost(Constraint):
    """Distance from the last pick back to the starting point. Default: soft, weight 1.

    An empty trolley never leaves, so it costs nothing."""

    name = "distance_from_last_pick_task_to_path_origin_cost"
    label_en = "Distance back to the start"
    label_ko = "출발 위치로 돌아오는 거리"
    description_en = (
        "The distance from a trolley's last pick back to its start location, in meters. "
        "One unit of cost is one meter; 0 means every trolley ends its round where it "
        "started (an empty trolley never leaves)."
    )
    description_ko = (
        "트롤리의 마지막 피킹 지점에서 출발 위치로 돌아오는 거리(미터)를 더합니다. 1미터가 비용 1이며, "
        "0이면 모든 트롤리가 출발한 곳에서 피킹을 마칩니다(빈 트롤리는 출발하지 않습니다)."
    )


class OrderSplitByTrolleyCost(Constraint):
    """Number of (order, trolley) pairs. Default: soft, weight 1000.

    An order shared by three trolleys costs three times one carried by a single
    trolley. Deliberately expensive relative to the distance terms — orders arriving
    in pieces become a packing problem downstream. Unlike the other constraints the
    raw cost is never 0 while picks are assigned: an order carried whole by one
    trolley still counts 1 (the quickstart's definition, kept)."""

    name = "order_split_by_trolley_cost"
    label_en = "Order-trolley pairs (order splits)"
    label_ko = "주문-트롤리 쌍(주문 분할)"
    description_en = (
        "The number of (order, trolley) pairs: for each trolley, how many different orders "
        "have a pick on it. An order kept on one trolley counts 1 and one spread over three "
        "trolleys counts 3, so the minimum is the number of orders with assigned picks; "
        "0 only when no pick is assigned."
    )
    description_ko = (
        "(주문, 트롤리) 쌍의 수를 셉니다. 트롤리마다 그 트롤리에 피킹이 하나라도 있는 서로 다른 "
        "주문의 수를 더한 값입니다. 한 트롤리에 모인 주문은 1, 트롤리 세 대에 나뉜 주문은 3이므로 "
        "최솟값은 피킹이 배정된 주문의 수이고, 배정된 피킹이 없을 때만 0입니다."
    )


#: Ready-made default instances (``trolley_capacity_cost`` is ``TrolleyCapacityCost()``).
trolley_capacity_cost = TrolleyCapacityCost()
distance_from_previous_pick_task_cost = DistanceFromPreviousPickTaskCost()
distance_from_last_pick_task_to_path_origin_cost = DistanceFromLastPickTaskToPathOriginCost()
order_split_by_trolley_cost = OrderSplitByTrolleyCost()

# The default set, in list_constraints() order.
_DEFAULTS = (
    trolley_capacity_cost,
    distance_from_previous_pick_task_cost,
    distance_from_last_pick_task_to_path_origin_cost,
    order_split_by_trolley_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all four default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([TrolleyCapacityCost(), OrderSplitByTrolleyCost(weight=100)])

    Same contract as the C++ applyConstraints: an empty list is an error (a
    filter that came out empty must not silently flip to "score everything");
    a constraint named several times is scored once per pick, each with its
    own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `op.constraint_costs(solution)` returns: per constraint, the
    **raw** cost of each trolley (before weight, exponent, offset and level
    rounding). Fetch it once and rescore with different weights without re-solving::

        raw = op.constraint_costs(solution)
        score(raw)                     # default set
        score(raw, [TrolleyCapacityCost(), OrderSplitByTrolleyCost(weight=100)])
        score(raw, [OrderSplitByTrolleyCost(), OrderSplitByTrolleyCost(weight=500, offset=2)])

    No solver and no search are involved. The arithmetic matches the engine —
    each trolley contributes ``weight * shape(cost, exponent, offset, clip)`` and
    hard contributions are **rounded per unit** before summing (the same order as
    the engine's applyRaw; rounding the total afterwards gives different values).
    A second pick of a constraint reads the same raw costs with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    return score_costs(costs, get_constraints(constraints))
