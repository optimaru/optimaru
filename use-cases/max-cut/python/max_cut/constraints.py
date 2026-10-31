"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`max_cut.solve(problem)`) scores every default constraint at
its default weight. This module is the alternative path next to it: pick
constraints, change weights, flip hard/soft, and pass the list to
`solve(problem, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import max_cut
    from max_cut.constraints import PartitionImbalanceCost, cut_weight_cost, get_constraints

    max_cut.solve(problem, constraints=get_constraints())                       # defaults as-is
    max_cut.solve(problem, constraints=[*get_constraints()[:4], PartitionImbalanceCost(weight=2), cut_weight_cost])

Default instances (`fixed_partition_cost`, `same_partition_pairs_cost`, ...,
`cut_weight_cost`) live at module level, so you can also pass a list of just a
few. Only listed constraints are scored — leave one out and its term disappears
from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (1, except ``partition_imbalance_cost``: 0, an opt-in
               objective). The problem carries no weights;
               ``max_cut.constraint_info()`` reports the defaults.
- ``hard``     ``True`` = hard, ``False`` = soft, ``None`` = the default level.
               ``cut_weight_cost`` is a reward and always soft: ``hard=True``
               raises ValueError.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| ** exponent``.
               The four unit constraints (fixed, same-side, different-side,
               size bounds) shape each SIDE's cost; the two globals (imbalance,
               cut) their single total.

Names (`name`) are the C++ component names in snake_case
(``FixedPartitionCost`` -> ``fixed_partition_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`max_cut.list_constraints()`.
"""

from ._constraint import Constraint, Score, score_costs, select

__all__ = [
    "Constraint",
    "Score",
    "score",
    "FixedPartitionCost",
    "SamePartitionPairsCost",
    "DifferentPartitionPairsCost",
    "PartitionSizeBoundsCost",
    "PartitionImbalanceCost",
    "CutWeightCost",
    "fixed_partition_cost",
    "same_partition_pairs_cost",
    "different_partition_pairs_cost",
    "partition_size_bounds_cost",
    "partition_imbalance_cost",
    "cut_weight_cost",
    "get_constraints",
]


class FixedPartitionCost(Constraint):
    """Fixed vertices on the other side, per side. Default: hard, weight 1."""

    name = "fixed_partition_cost"
    label_en = "Fixed partition"
    label_ko = "고정 파티션"
    description_en = (
        "Vertices pinned by fixed_partition that sit on the other side, counted on the side they sit on. "
        "One unit of cost is one misplaced fixed vertex; 0 means every fixed vertex is on its side. "
        "The search never moves a fixed vertex, so only a warm-start seed can make it positive."
    )
    description_ko = (
        "fixed_partition으로 고정한 정점이 반대쪽 파티션에 놓인 수를, 정점이 놓인 쪽에서 셉니다. "
        "잘못 놓인 고정 정점 하나가 비용 1이며, 0이면 모든 고정 정점이 제자리에 있습니다. "
        "탐색은 고정 정점을 옮기지 않으므로 웜 스타트로 넣은 초기 분할만 이 값을 0보다 크게 만들 수 있습니다."
    )


class SamePartitionPairsCost(Constraint):
    """Must-be-together pairs that are split, per side of the first vertex. Default: hard, weight 1."""

    name = "same_partition_pairs_cost"
    label_en = "Same-side pairs"
    label_ko = "같은 쪽 쌍"
    description_en = (
        "Pairs in same_partition_pairs whose vertices end up on different sides; each split pair is counted "
        "once, on the side of its first vertex. One unit of cost is one split pair; 0 means every pair is together."
    )
    description_ko = (
        "same_partition_pairs의 두 정점이 서로 다른 쪽에 놓인 쌍의 수입니다. 갈라진 쌍은 첫 번째 정점이 놓인 쪽에서 "
        "한 번만 셉니다. 갈라진 쌍 하나가 비용 1이며, 0이면 모든 쌍의 두 정점이 같은 쪽에 있습니다."
    )


class DifferentPartitionPairsCost(Constraint):
    """Must-be-apart pairs on one side, per side. Default: hard, weight 1."""

    name = "different_partition_pairs_cost"
    label_en = "Different-side pairs"
    label_ko = "다른 쪽 쌍"
    description_en = (
        "Pairs in different_partition_pairs whose vertices end up on the same side, counted on that side. "
        "One unit of cost is one pair left together; 0 means every pair is split across the cut."
    )
    description_ko = (
        "different_partition_pairs의 두 정점이 같은 쪽에 놓인 쌍의 수를 그쪽에서 셉니다. "
        "함께 놓인 쌍 하나가 비용 1이며, 0이면 모든 쌍이 서로 다른 쪽으로 나뉘어 있습니다."
    )


class PartitionSizeBoundsCost(Constraint):
    """Vertices a side has below the minimum or above the maximum size. Default: hard, weight 1."""

    name = "partition_size_bounds_cost"
    label_en = "Partition size bounds"
    label_ko = "파티션 크기 범위"
    description_en = (
        "For each side, how many vertices it has fewer than min_partition_size or more than max_partition_size. "
        "One unit of cost is one vertex outside the bounds; 0 means both sides are within them."
    )
    description_ko = (
        "각 쪽의 정점 수가 min_partition_size보다 모자라거나 max_partition_size를 넘는 만큼입니다. "
        "범위를 벗어난 정점 하나가 비용 1이며, 0이면 두 쪽 모두 범위 안에 있습니다."
    )


class PartitionImbalanceCost(Constraint):
    """Size difference between the two sides, an opt-in objective. Default: soft, weight 0."""

    name = "partition_imbalance_cost"
    label_en = "Partition imbalance"
    label_ko = "파티션 불균형"
    description_en = (
        "The absolute difference between the numbers of vertices on the two sides. One unit of cost is one vertex "
        "of difference; 0 means the sides are equal. Its default weight is 0, so it only counts once you give it "
        "a weight."
    )
    description_ko = (
        "두 쪽에 놓인 정점 수의 차이(절댓값)입니다. 정점 하나만큼의 차이가 비용 1이며, 0이면 두 쪽의 크기가 같습니다. "
        "기본 가중치가 0이므로 가중치를 주어야 점수에 반영됩니다."
    )


class CutWeightCost(Constraint):
    """Total weight of the cut edges, a reward (raw cost <= 0). Default: soft, weight 1; never hard."""

    name = "cut_weight_cost"
    label_en = "Cut weight"
    label_ko = "컷 가중치"
    description_en = (
        "The total weight of the edges whose endpoints are on different sides, scored as a reward: the raw cost is "
        "minus the cut weight (never positive), so a larger cut scores better. It is always soft; hard=True is refused."
    )
    description_ko = (
        "양 끝 정점이 서로 다른 쪽에 놓인 간선의 가중치 합을 보상으로 셉니다. 원시 비용은 컷 가중치에 음수를 붙인 "
        "값(0 이하)이므로 컷이 클수록 점수가 좋아집니다. 항상 소프트 제약이며 hard=True는 거부됩니다."
    )


#: Instances at the default weights (weight=None). Reference them directly when picking.
fixed_partition_cost = FixedPartitionCost()
same_partition_pairs_cost = SamePartitionPairsCost()
different_partition_pairs_cost = DifferentPartitionPairsCost()
partition_size_bounds_cost = PartitionSizeBoundsCost()
partition_imbalance_cost = PartitionImbalanceCost()
cut_weight_cost = CutWeightCost()

# The default set, in list_constraints() order.
_DEFAULTS = (
    fixed_partition_cost,
    same_partition_pairs_cost,
    different_partition_pairs_cost,
    partition_size_bounds_cost,
    partition_imbalance_cost,
    cut_weight_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, every default constraint.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([*get_constraints()[:4], PartitionImbalanceCost(weight=2), cut_weight_cost])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) and a constraint named twice raise ValueError.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `max_cut.constraint_costs(problem)` returns: per constraint
    its **raw** costs (per side for the four unit constraints, one total for the
    imbalance and the cut), before weight, exponent and level rounding. Fetch it
    once and rescore with different weights without re-solving::

        raw = max_cut.constraint_costs(problem)
        score(raw)                                              # default set
        score(raw, [partition_size_bounds_cost, PartitionImbalanceCost(weight=2), cut_weight_cost])

    No solver and no search are involved. The arithmetic matches the engine —
    each side (or global total) contributes ``weight * shape(cost, exponent)`` and
    hard contributions are **rounded per side** before summing (the same order as
    the engine; rounding the total afterwards gives different values for
    fractional hard weights).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError; ``hard=True``
    on the ``cut_weight_cost`` reward raises ValueError.
    """
    return score_costs(costs, get_constraints(constraints))
