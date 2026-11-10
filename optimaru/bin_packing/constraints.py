"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`bp.solve(problem)`) enables all four constraints at their default
weights. This module is the alternative path next to it: pick constraints, change
weights, change levels, shape the cost, and pass the list to
`solve(problem, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import optimaru.bin_packing as bp
    from optimaru.bin_packing.constraints import BinCapacityCost, UsedBinsCost, get_constraints

    bp.solve(problem, constraints=get_constraints())              # the defaults as-is
    bp.solve(problem, constraints=[                                # picked
        BinCapacityCost(), UsedBinsCost(weight=10),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``bin_capacity_cost``
is ``BinCapacityCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (1 for every bin-packing constraint). The problem carries no
               weights; ``bp.constraint_info()`` reports the defaults.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level: ``level="medium"`` raises ValueError.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| ** exponent``.
               Applied per BIN for the three per-bin constraints, and on the bin
               count for UsedBinsCost.
- ``offset``   (keyword) shifts that shaping, at the same granularity; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a bin below the offset — an empty one
               (cost 0) included, every bin 0 .. item count - 1 — lowers the
               penalty, and a bin count below it does too. With an offset it is
               refused on hard (the three per-bin constraints by default), because
               hard 0 must mean no violation.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs (its level included) and the picks add up. Every bin costs 1, and every bin beyond the tenth
100 on top::

    bp.solve(problem, constraints=[
        *get_constraints(),
        UsedBinsCost(weight=100, offset=10),
    ])

``bp.constraint_costs(problem, picks)`` keys the second pick ``"used_bins_cost#2"``.

Names (`name`) are the C++ component names in snake_case
(``BinCapacityCost`` -> ``bin_capacity_cost``). The core derives them mechanically,
so there is no hand-maintained alias table — check them with `bp.list_constraints()`.
"""

from .._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "BinCapacityCost",
    "ColorConflictCost",
    "ItemConflictCost",
    "UsedBinsCost",
    "bin_capacity_cost",
    "color_conflict_cost",
    "item_conflict_cost",
    "used_bins_cost",
    "get_constraints",
]


class BinCapacityCost(Constraint):
    """Load above the bin capacity, per bin. Default: hard, weight 1."""

    name = "bin_capacity_cost"
    label_en = "Bin capacity"
    label_ko = "상자 용량"
    description_en = (
        "The weight packed into a bin beyond its capacity, summed over the bins. "
        "One unit of cost is one unit of weight over capacity; 0 means every bin is within its capacity."
    )
    description_ko = (
        "상자마다 담긴 무게 가운데 용량을 넘는 양을 모두 더합니다. "
        "초과 무게 1단위가 비용 1이며, 0이면 어느 상자도 용량을 넘지 않습니다."
    )


class ColorConflictCost(Constraint):
    """Items sharing a bin with another item of the same colour. Default: hard, weight 1.

    Per bin: every item beyond the first of its colour. The exponent shapes each bin's
    sum (red x3 + blue x2 in one bin is 3, so exponent 2 gives 9)."""

    name = "color_conflict_cost"
    label_en = "Color conflict"
    label_ko = "색상 충돌"
    description_en = (
        "Items packed into a bin that already holds an item of the same colour: per bin, every item "
        "beyond the first of its colour counts 1. Items without a colour never count; "
        "0 means no bin holds two items of one colour."
    )
    description_ko = (
        "같은 색 물건이 이미 담긴 상자에 더 담긴 물건의 수입니다. 상자마다 색별로 첫 번째 물건을 뺀 "
        "나머지 물건 하나당 1입니다. 색이 없는 물건은 세지 않으며, 0이면 같은 색 물건 두 개가 "
        "한 상자에 담긴 경우가 없습니다."
    )


class ItemConflictCost(Constraint):
    """Conflict pairs placed in one bin. Default: hard, weight 1.

    Per bin: the listed pairs with both items in it (a pair listed twice counts twice).
    The exponent shapes each bin's count."""

    name = "item_conflict_cost"
    label_en = "Item conflicts"
    label_ko = "물건 충돌"
    description_en = (
        "Conflict pairs whose two items share a bin, one per listed pair. "
        "0 means every conflicting pair is packed apart."
    )
    description_ko = (
        "함께 담으면 안 되는 물건 쌍 가운데 두 물건이 같은 상자에 담긴 쌍의 수입니다. 목록에 적힌 "
        "쌍 하나당 1이며, 0이면 모든 충돌 쌍이 서로 다른 상자에 담겨 있습니다."
    )


class UsedBinsCost(Constraint):
    """Number of bins in use (the objective). Default: soft, weight 1.

    One global value; the exponent shapes the count itself (count ** exponent)."""

    name = "used_bins_cost"
    label_en = "Bins used"
    label_ko = "사용한 상자 수"
    description_en = "The number of bins holding at least one item. 0 means no item is packed."
    description_ko = "물건이 하나라도 담긴 상자의 수입니다. 0이면 담긴 물건이 없습니다."


#: Ready-made default instances (``bin_capacity_cost`` is ``BinCapacityCost()``).
bin_capacity_cost = BinCapacityCost()
color_conflict_cost = ColorConflictCost()
item_conflict_cost = ItemConflictCost()
used_bins_cost = UsedBinsCost()

# The default set, in list_constraints() order.
_DEFAULTS = (bin_capacity_cost, color_conflict_cost, item_conflict_cost, used_bins_cost)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all four default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([BinCapacityCost(), ColorConflictCost(), ItemConflictCost(), UsedBinsCost(weight=10)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) raises ValueError; a constraint named several times is
    scored once per pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `bp.constraint_costs(problem)` returns: per constraint, the
    **raw** cost of each bin (``used_bins_cost``: the one bin count), before weight,
    exponent, offset and level rounding. Fetch it once and rescore with different
    weights without re-solving::

        raw = bp.constraint_costs(problem)
        score(raw)                                    # default set
        score(raw, [BinCapacityCost(), UsedBinsCost(weight=10)])
        score(raw, [UsedBinsCost(), UsedBinsCost(weight=100, offset=10)])

    No solver and no search are involved. The arithmetic matches the engine —
    each bin contributes ``weight * shape(cost, exponent, offset, clip)`` and hard
    contributions are **rounded per bin** before summing (the same order as the
    engine). A second pick of a constraint reads the same raw costs with its own
    knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    return score_costs(costs, get_constraints(constraints))
