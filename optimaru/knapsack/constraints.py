"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`knapsack.solve(problem)`) scores every default constraint at
its default weight. This module is the alternative path next to it: pick
constraints, change weights, change levels, shape the cost, and pass the list to
`solve(problem, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    from optimaru import knapsack
    from optimaru.knapsack.constraints import (
        KnapsackCapacityCost, MandatoryItemsCost, SelectedValueCost, get_constraints,
    )

    solved = knapsack.solve(problem, constraints=get_constraints())  # defaults as-is
    solved = knapsack.solve(problem, constraints=[*get_constraints()[:-1], SelectedValueCost(weight=2)])
    solved = knapsack.solve(problem, constraints=[  # picked
        KnapsackCapacityCost(), MandatoryItemsCost(), SelectedValueCost(),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances:
``knapsack_capacity_cost`` is ``KnapsackCapacityCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (1 for every knapsack constraint). The problem carries no
               weights; ``knapsack.constraint_info()`` reports the defaults.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level: ``level="medium"`` raises ValueError.
               ``SelectedValueCost`` is a reward and always soft:
               ``level="hard"`` raises ValueError.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| ** exponent``
               on each constraint's single total (every knapsack constraint is
               global), e.g. the summed resource overflow.
- ``offset``   (keyword) shifts that shaping of the total; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a total below the offset — a raw 0
               included — lowers the penalty. With an offset it is refused on
               hard (the six penalties by default), because hard 0 must mean no
               violation. On the ``SelectedValueCost`` reward
               (raw ``-value``) a threshold offset rewards only the value beyond it,
               a literal one adds ``offset`` to the reward.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs (its level included) and the picks add up. The resource overflow plus its
square beyond 10 units::

    solved = knapsack.solve(problem, constraints=[
        KnapsackCapacityCost(),
        ResourceCapacityCost(),
        ResourceCapacityCost(exponent=2, offset=10),
        SelectedValueCost(),
    ])

``knapsack.constraint_costs(problem, picks)`` keys the second pick
``"resource_capacity_cost#2"``.

Names (`name`) are the C++ component names in snake_case
(``KnapsackCapacityCost`` -> ``knapsack_capacity_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`knapsack.list_constraints()`.
"""

from .._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "ForbiddenItemsCost",
    "ItemDependencyCost",
    "KnapsackCapacityCost",
    "Level",
    "MandatoryItemsCost",
    "ResourceCapacityCost",
    "Score",
    "SelectedValueCost",
    "SelectionGroupBoundsCost",
    "forbidden_items_cost",
    "get_constraints",
    "item_dependency_cost",
    "knapsack_capacity_cost",
    "mandatory_items_cost",
    "resource_capacity_cost",
    "score",
    "selected_value_cost",
    "selection_group_bounds_cost",
]


class KnapsackCapacityCost(Constraint):
    """Selected weight above the knapsack capacity. Default: hard, weight 1."""

    name = "knapsack_capacity_cost"
    label_en = "Knapsack capacity"
    label_ko = "배낭 용량"
    description_en = (
        "Total weight of the selected items beyond the knapsack's capacity. "
        "One unit of cost is one unit of weight over capacity; 0 means the selection fits."
    )
    description_ko = (
        "선택한 물품의 무게 합이 배낭 용량을 넘는 양입니다. "
        "초과 무게 1단위가 비용 1이며, 0이면 선택한 물품이 용량 안에 들어갑니다."
    )


class ResourceCapacityCost(Constraint):
    """Selected usage above each declared resource capacity. Default: hard, weight 1."""

    name = "resource_capacity_cost"
    label_en = "Resource capacity"
    label_ko = "자원 용량"
    description_en = (
        "Usage of each declared resource beyond its capacity, summed over the resources; usage of a resource "
        "without a capacity is ignored. One unit of cost is one unit of a resource over its capacity; "
        "0 means every resource fits."
    )
    description_ko = (
        "용량이 정해진 자원마다 선택한 물품의 사용량이 용량을 넘는 양을 모두 더하며, 용량이 없는 자원의 사용량은 "
        "무시합니다. 초과 사용량 1단위가 비용 1이며, 0이면 모든 자원의 사용량이 용량 안에 있습니다."
    )


class MandatoryItemsCost(Constraint):
    """Mandatory items left out. Default: hard, weight 1."""

    name = "mandatory_items_cost"
    label_en = "Mandatory items"
    label_ko = "필수 물품"
    description_en = (
        "Mandatory items that are not selected; an item listed twice counts twice. "
        "One unit of cost is one missing entry; 0 means every mandatory item is selected."
    )
    description_ko = (
        "선택되지 않은 필수 물품의 수이며, 목록에 두 번 들어 있는 물품은 두 번 셉니다. "
        "빠진 항목 하나가 비용 1이며, 0이면 필수 물품이 모두 선택되어 있습니다."
    )


class ForbiddenItemsCost(Constraint):
    """Forbidden items selected. Default: hard, weight 1."""

    name = "forbidden_items_cost"
    label_en = "Forbidden items"
    label_ko = "금지 물품"
    description_en = (
        "Forbidden items that are selected; an item listed twice counts twice. "
        "One unit of cost is one selected entry; 0 means no forbidden item is selected."
    )
    description_ko = (
        "선택된 금지 물품의 수이며, 목록에 두 번 들어 있는 물품은 두 번 셉니다. "
        "선택된 항목 하나가 비용 1이며, 0이면 선택된 금지 물품이 없습니다."
    )


class SelectionGroupBoundsCost(Constraint):
    """Selection groups below their minimum or above their maximum. Default: hard, weight 1."""

    name = "selection_group_bounds_cost"
    label_en = "Selection group bounds"
    label_ko = "선택 그룹 범위"
    description_en = (
        "How far each selection group's number of selected items falls below its minimum or rises above its "
        "maximum, summed over the groups. One unit of cost is one item too few or too many; "
        "0 means every group is within its bounds."
    )
    description_ko = (
        "선택 그룹마다 선택된 물품 수가 최솟값에 못 미치거나 최댓값을 넘는 만큼을 모두 더합니다. "
        "물품이 하나 모자라거나 남을 때마다 비용 1이며, 0이면 모든 그룹의 선택 수가 범위 안에 있습니다."
    )


class ItemDependencyCost(Constraint):
    """Selected items whose required item is left out. Default: hard, weight 1."""

    name = "item_dependency_cost"
    label_en = "Item dependency"
    label_ko = "물품 의존 관계"
    description_en = (
        "Selected items whose required item is not selected, counted per (item, required item) pair. "
        "One unit of cost is one unmet requirement; 0 means every selected item has what it requires."
    )
    description_ko = (
        "선택된 물품이 필요로 하는 물품이 선택되지 않은 경우를 (물품, 필요 물품) 쌍마다 셉니다. "
        "충족되지 않은 쌍 하나가 비용 1이며, 0이면 선택된 모든 물품의 필요 물품이 함께 선택되어 있습니다."
    )


class SelectedValueCost(Constraint):
    """Total value of the selection, a reward (raw cost <= 0). Default: soft, weight 1; never hard."""

    name = "selected_value_cost"
    label_en = "Selected value"
    label_ko = "선택 물품 가치"
    description_en = (
        "The total value of the selected items, scored as a reward: the raw cost is minus the selected value "
        "(never positive), so a larger value scores better. It is always soft; level='hard' is refused."
    )
    description_ko = (
        "선택한 물품의 가치 합을 보상으로 셉니다. 원시 비용은 이 가치 합에 음수를 붙인 값(0 이하)이므로 "
        "가치가 클수록 점수가 좋아집니다. 항상 소프트 제약이며 level='hard'는 거부됩니다."
    )


#: Ready-made default instances (``knapsack_capacity_cost`` is ``KnapsackCapacityCost()``).
knapsack_capacity_cost = KnapsackCapacityCost()
resource_capacity_cost = ResourceCapacityCost()
mandatory_items_cost = MandatoryItemsCost()
forbidden_items_cost = ForbiddenItemsCost()
selection_group_bounds_cost = SelectionGroupBoundsCost()
item_dependency_cost = ItemDependencyCost()
selected_value_cost = SelectedValueCost()

# The default set, in list_constraints() order.
_DEFAULTS = (
    knapsack_capacity_cost,
    resource_capacity_cost,
    mandatory_items_cost,
    forbidden_items_cost,
    selection_group_bounds_cost,
    item_dependency_cost,
    selected_value_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, every default constraint.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, list the others as they are::

        get_constraints([*get_constraints()[:-1], SelectedValueCost(weight=2)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) raises ValueError; a constraint named several times is
    scored once per pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `knapsack.constraint_costs(problem)` returns: per constraint
    its **raw** total (a one-element list), before weight, exponent, offset and level
    rounding. Fetch it once and rescore with different weights without re-solving::

        raw = knapsack.constraint_costs(problem)
        score(raw)                                              # default set
        score(raw, [KnapsackCapacityCost(), SelectedValueCost(weight=2)])
        score(raw, [MandatoryItemsCost(), MandatoryItemsCost(exponent=2, offset=1)])

    No solver and no search are involved. The arithmetic matches the engine —
    each constraint contributes ``weight * shape(cost, exponent, offset, clip)`` and
    hard contributions are **rounded per constraint** before summing (the same order
    as the engine; rounding the total afterwards gives different values for
    fractional hard weights). A second pick of a constraint reads the same raw total
    with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError; so do
    ``level="medium"`` (no medium level here) and ``level="hard"`` on the
    ``selected_value_cost`` reward.
    """
    return score_costs(costs, get_constraints(constraints))
