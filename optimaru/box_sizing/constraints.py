"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`bs.solve(problem)`) scores the five default constraints at their default
weights (the sixth, `box_type_cost`, is opt-in: weight 0). This module is the alternative
path next to it: pick constraints, change weights, change levels, shape the cost, and pass
the list to `solve(problem, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import optimaru.box_sizing as bs
    from optimaru.box_sizing.constraints import BoxTypeCost, VoidVolumeCost, get_constraints

    bs.solve(problem, constraints=get_constraints())               # the defaults as-is
    bs.solve(problem, constraints=[                                 # one default changed
        BoxTypeCost(weight=2_000_000) if spec.name == "box_type_cost" else spec
        for spec in get_constraints()
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``void_volume_cost``
is ``VoidVolumeCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (1 for all but BoxTypeCost, which is 0: off until picked
               with a weight). The problem carries no weights;
               ``bs.constraint_info()`` reports the defaults.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| ** exponent``,
               per box for the four per-box constraints and on the one count for
               BoxTypeLimitCost and BoxTypeCost.
- ``offset``   (keyword) shifts that shaping; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is. With an offset it is refused on hard,
               because hard 0 must mean no violation.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs and the picks add up. ``bs.constraint_costs(problem, picks)`` keys the
second pick ``"void_volume_cost#2"``.

Names (`name`) are the C++ component names in snake_case
(``VoidVolumeCost`` -> ``void_volume_cost``). The core derives them mechanically, so
there is no hand-maintained alias table — check them with `bs.list_constraints()`.
"""

from .._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "BoxCost",
    "BoxTypeCost",
    "BoxTypeLimitCost",
    "BoxWeightCost",
    "Constraint",
    "Level",
    "ParcelFitCost",
    "Score",
    "VoidVolumeCost",
    "box_cost",
    "box_type_cost",
    "box_type_limit_cost",
    "box_weight_cost",
    "get_constraints",
    "parcel_fit_cost",
    "score",
    "void_volume_cost",
]


class ParcelFitCost(Constraint):
    """A parcel must fit its box in some rotation. Default: hard, weight 1."""

    name = "parcel_fit_cost"
    label_en = "Parcel fits its box"
    label_ko = "상자에 들어가는 크기"
    description_en = (
        "For each box, over the parcels that ship in it, the length by which the parcel exceeds the box, "
        "summed over the three sides sorted by length (so any rotation counts). "
        "0 means every parcel fits its box."
    )
    description_ko = (
        "상자마다 그 상자로 나가는 화물이 상자보다 긴 만큼을, 세 변을 길이순으로 맞대어 모두 더합니다"
        "(어느 방향으로 돌려도 됩니다). 0이면 모든 화물이 상자에 들어갑니다."
    )


class BoxWeightCost(Constraint):
    """A parcel may not weigh more than its box carries. Default: hard, weight 1."""

    name = "box_weight_cost"
    label_en = "Box weight limit"
    label_ko = "상자 허용 무게"
    description_en = (
        "For each box, over the parcels that ship in it, the parcel's weight above the box's max_weight. "
        "0 means no parcel is too heavy for its box; a box without a max_weight never costs."
    )
    description_ko = (
        "상자마다 그 상자로 나가는 화물의 무게가 상자의 허용 무게(max_weight)를 넘는 만큼을 모두 더합니다. "
        "0이면 허용 무게를 넘는 화물이 없으며, 허용 무게가 없는 상자는 비용이 없습니다."
    )


class VoidVolumeCost(Constraint):
    """The air shipped: box volume not filled by the parcel. Default: soft, weight 1."""

    name = "void_volume_cost"
    label_en = "Void volume"
    label_ko = "빈 공간 부피"
    description_en = (
        "For each box, over the parcels that ship in it, quantity x (box volume - parcel volume): "
        "the air shipped. 0 means every parcel fills its box."
    )
    description_ko = (
        "상자마다 그 상자로 나가는 화물의 수량 x (상자 부피 - 화물 부피)를 모두 더합니다. "
        "배송되는 빈 공간이며, 0이면 모든 화물이 상자를 가득 채웁니다."
    )


class BoxCost(Constraint):
    """What the boxes shipped cost. Default: soft, weight 1.

    It shares the soft level with the void volume, so its weight says how much
    volume one unit of cost is worth."""

    name = "box_cost"
    label_en = "Box cost"
    label_ko = "상자 비용"
    description_en = (
        "For each box, over the parcels that ship in it, quantity x the box's cost. "
        "0 means the boxes in use cost nothing."
    )
    description_ko = (
        "상자마다 그 상자로 나가는 화물의 수량 x 상자 단가(cost)를 모두 더합니다. 0이면 쓰는 상자에 비용이 없습니다."
    )


class BoxTypeLimitCost(Constraint):
    """At most max_box_types distinct boxes may be in use. Default: hard, weight 1.

    A step function of "is this box used at all", which is why the solver has
    its own moves that close, open and swap a size."""

    name = "box_type_limit_cost"
    label_en = "Box type limit"
    label_ko = "상자 종류 수 제한"
    description_en = (
        "The number of distinct boxes in use (those a parcel ships in and the required ones) above "
        "max_box_types. 0 means the limit holds; a problem without max_box_types never costs."
    )
    description_ko = (
        "쓰는 상자 종류(화물이 나가는 상자와 required 상자)의 수가 max_box_types를 넘는 만큼입니다. "
        "0이면 제한을 지키며, max_box_types가 없으면 비용이 없습니다."
    )


class BoxTypeCost(Constraint):
    """The number of distinct boxes in use. Default: soft, weight 0 (opt-in).

    Pick it with a weight to give every size a fixed cost: the weight is what
    stocking one more size costs, in the units of the soft level."""

    name = "box_type_cost"
    label_en = "Box types in use"
    label_ko = "쓰는 상자 종류 수"
    description_en = (
        "The number of distinct boxes in use (those a parcel ships in and the required ones). "
        "Its weight is the fixed cost of stocking one more size; at the default weight 0 it scores nothing."
    )
    description_ko = (
        "쓰는 상자 종류(화물이 나가는 상자와 required 상자)의 수입니다. "
        "가중치가 상자 종류 하나를 더 두는 고정 비용이며, 기본 가중치 0에서는 점수에 반영되지 않습니다."
    )


#: Ready-made default instances (``void_volume_cost`` is ``VoidVolumeCost()``).
parcel_fit_cost = ParcelFitCost()
box_weight_cost = BoxWeightCost()
void_volume_cost = VoidVolumeCost()
box_cost = BoxCost()
box_type_limit_cost = BoxTypeLimitCost()
box_type_cost = BoxTypeCost()

# The default set, in list_constraints() order.
_DEFAULTS = (
    parcel_fit_cost,
    box_weight_cost,
    void_volume_cost,
    box_cost,
    box_type_limit_cost,
    box_type_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all six constraints at their defaults
    (`box_type_cost` at weight 0, so five of them score).

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, list the others as they are::

        [BoxCost(weight=1000) if spec.name == "box_cost" else spec for spec in get_constraints()]

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) raises ValueError; a constraint named several times is
    scored once per pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `bs.constraint_costs(problem)` returns: per constraint, the
    **raw** cost of each box, or the one count of the two box-type constraints (before
    weight, exponent, offset and level rounding). Fetch it once and rescore with
    different weights without re-solving::

        raw = bs.constraint_costs(problem)
        score(raw)                                    # default set
        score(raw, [VoidVolumeCost(), BoxTypeCost(weight=2_000_000)])

    No solver and no search are involved. The arithmetic matches the engine —
    each box contributes ``weight * shape(cost, exponent, offset, clip)`` and
    hard contributions are **rounded per box** before summing. A second pick of a
    constraint reads the same raw costs with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    return score_costs(costs, get_constraints(constraints))
