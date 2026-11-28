"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`cl.solve(problem)`) scores all fifteen constraints at their default
weights, two of them opt-in at weight 0. This module is how a call changes that: pick
constraints, change weights, change levels, shape the cost, and pass the list to
`solve(problem, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import optimaru.container_loading as cl
    from optimaru.container_loading.constraints import (
        ContainerWeightCost, UnplacedItemCost, UsedContainerCost, UsedLengthCost, get_constraints,
    )

    cl.solve(problem, constraints=get_constraints())           # the defaults as-is
    cl.solve(problem, constraints=[                            # picked
        UnplacedItemCost(), ContainerWeightCost(), UsedContainerCost(), UsedLengthCost(weight=0.001),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. To keep the defaults and change one, replace it by name::

    picks = [UsedLengthCost(weight=0.001) if spec.name == "used_length_cost" else spec
             for spec in get_constraints()]

(The module also carries ready-made default instances: ``unplaced_item_cost`` is
``UnplacedItemCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (1, and 0 for the two opt-in costs `UsedLengthCost` and
               `LoadBalanceCost`); ``cl.constraint_info()`` reports the defaults.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level: ``level="medium"`` raises ValueError.
- ``exponent`` sign-preserving power shaping per container: ``weight * sign(c) *
               |c| ** exponent``. 2.0 = squared, so concentrated violations grow
               much costlier.
- ``offset``   (keyword) shifts that shaping per container; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a container below the offset — an empty
               one (cost 0) included — lowers the penalty. With an offset it is
               refused on hard, because hard 0 must mean no violation.

`GroupSplitCost`, `IncompleteGroupCost` and `SingleContainerCost` are the global
constraints: their knobs shape the single total, not a cost per container.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs and the picks add up. ``cl.constraint_costs(problem, picks)`` keys a
second pick ``"<name>#2"``.

Names (`name`) are the C++ component names in snake_case
(``UnplacedItemCost`` -> ``unplaced_item_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`cl.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.

With a selection the solve is single-start: `run.parallel_solver_count` is ignored. The
search is the same one, the domain's moves included.
"""

from .._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "AxleLoadCost",
    "BalanceLimitCost",
    "Constraint",
    "ContainerWeightCost",
    "GroupSplitCost",
    "IncompleteGroupCost",
    "Level",
    "LoadBalanceCost",
    "OptionalItemCost",
    "Score",
    "SegregationCost",
    "SingleContainerCost",
    "StackingLoadCost",
    "StackingRuleCost",
    "UnloadingOrderCost",
    "UnplacedItemCost",
    "UsedContainerCost",
    "UsedLengthCost",
    "axle_load_cost",
    "balance_limit_cost",
    "container_weight_cost",
    "get_constraints",
    "group_split_cost",
    "incomplete_group_cost",
    "load_balance_cost",
    "optional_item_cost",
    "score",
    "segregation_cost",
    "single_container_cost",
    "stacking_load_cost",
    "stacking_rule_cost",
    "unloading_order_cost",
    "unplaced_item_cost",
    "used_container_cost",
    "used_length_cost",
]


class UnplacedItemCost(Constraint):
    """Items the decoder found no place for. Default: hard, weight 1.

    An item is unplaced when no free corner of its container takes it in an allowed
    orientation: inside the walls, clear of the other items, with enough of its base
    supported. It stays in the container's loading order and counts here until the
    search finds it another container or another place in the order."""

    name = "unplaced_item_cost"
    label_en = "Items without a place"
    label_ko = "자리를 찾지 못한 물품"
    description_en = (
        "The items of a container's loading order that could not be placed: no free position "
        "inside the container where the item fits in an allowed orientation without overlapping "
        "another item and with enough of its base supported. One unit of cost is one such item; "
        "0 means every item stands somewhere."
    )
    description_ko = (
        "컨테이너의 적재 순서에 있지만 놓을 자리를 찾지 못한 물품을 셉니다. 허용된 방향으로 "
        "컨테이너 안에 들어가고, 다른 물품과 겹치지 않으며, 바닥 면이 충분히 받쳐지는 위치가 "
        "없는 경우입니다. 물품 1개가 비용 1이며, 0이면 모든 물품이 자리를 잡았습니다."
    )


class ContainerWeightCost(Constraint):
    """Weight above a container's `max_weight`. Default: hard, weight 1.

    Counts the whole loading order of the container, in the caller's weight unit. A
    container without `max_weight` costs nothing."""

    name = "container_weight_cost"
    label_en = "Container weight limit"
    label_ko = "컨테이너 중량 한도"
    description_en = (
        "The total weight of the items given to a container above its max_weight, in the "
        "weight unit of the input. One unit of cost is one weight unit over the limit; 0 means "
        "no container is overloaded (a container without max_weight has no limit)."
    )
    description_ko = (
        "컨테이너에 배정된 물품의 총중량이 max_weight를 넘는 만큼을 입력의 중량 단위로 더합니다. "
        "한도를 넘은 중량 1단위가 비용 1이며, 0이면 과적된 컨테이너가 없습니다(max_weight가 "
        "없는 컨테이너는 한도가 없습니다)."
    )


class StackingLoadCost(Constraint):
    """Weight resting on an item above its `max_load_on_top`. Default: hard, weight 1.

    Every item passes its own weight and what rests on it down to the items right under
    it, shared by contact area. `max_load_on_top=0` means nothing may stand on the item."""

    name = "stacking_load_cost"
    label_en = "Stacking load limit"
    label_ko = "적층 하중 한도"
    description_en = (
        "The weight resting on an item above its max_load_on_top. An item passes its own "
        "weight and the load on it down to the items directly under it, in proportion to the "
        "contact area. One unit of cost is one weight unit over an item's limit (rounded up per "
        "item); 0 means no item carries more than it can."
    )
    description_ko = (
        "물품 위에 실린 하중이 그 물품의 max_load_on_top을 넘는 만큼을 더합니다. 각 물품은 자기 "
        "무게와 위에 실린 하중을 바로 아래 물품들에 접촉 면적에 비례해 전달합니다. 한도를 넘은 "
        "중량 1단위가 비용 1이며(물품별 올림), 0이면 어떤 물품도 견딜 수 있는 것보다 많이 "
        "받치고 있지 않습니다."
    )


class StackingRuleCost(Constraint):
    """Items standing on each other against a stacking rule, and columns above an item's
    `max_stack_layers`. Default: hard, weight 1.

    An item stands on another when its base rests on the other's top. Each such pair whose
    `stack_group`s a rule of the problem's `stacking_rules` forbids counts 1, and every layer
    a column through an item has beyond its `max_stack_layers` counts 1."""

    name = "stacking_rule_cost"
    label_en = "Stacking rules"
    label_ko = "적층 규칙"
    description_en = (
        "The pairs of items where one stands on the other although a stacking rule forbids "
        "its stack group on the other's, plus, for every item with max_stack_layers, the layers "
        "the tallest column through it has beyond that limit (the item, the items under it down "
        "to the floor and the items on top of it). One unit of cost is one such pair or one "
        "layer too many; 0 means every stacking rule and layer limit holds."
    )
    description_ko = (
        "적층 규칙이 금지한 적층 그룹의 물품 위에 놓인 물품 쌍을 세고, max_stack_layers가 있는 "
        "물품마다 그 물품을 지나는 가장 높은 기둥(그 물품, 그 아래 바닥까지의 물품, 그 위의 "
        "물품)이 한도를 넘는 단 수를 더합니다. 그런 쌍 하나나 넘친 단 하나가 비용 1이며, 0이면 "
        "모든 적층 규칙과 단수 한도가 지켜졌습니다."
    )


class UnloadingOrderCost(Constraint):
    """Items in the way of one that leaves earlier. Default: hard, weight 1.

    A lower `unload_order` leaves first (a route's stop number). An item blocks another
    when it stands between it and the door, or above it. Items without `unload_order`
    are not counted."""

    name = "unloading_order_cost"
    label_en = "Unloading order"
    label_ko = "하차 순서"
    description_en = (
        "The pairs of items in one container where an item that is unloaded later blocks one "
        "that is unloaded earlier: it stands between that item and the door, or on top of it. "
        "One unit of cost is one such pair; 0 means every item can be taken out towards the door "
        "when its stop comes. Items without unload_order are ignored."
    )
    description_ko = (
        "한 컨테이너 안에서 나중에 내릴 물품이 먼저 내릴 물품을 막고 있는 쌍을 셉니다. 그 물품과 "
        "문 사이에 있거나 그 위에 놓인 경우입니다. 한 쌍이 비용 1이며, 0이면 모든 물품을 자기 "
        "하차 순서에 문 쪽으로 꺼낼 수 있습니다. unload_order가 없는 물품은 세지 않습니다."
    )


class BalanceLimitCost(Constraint):
    """A load's centre of gravity beyond the container's `max_offset_length` and
    `max_offset_width`. Default: hard, weight 1.

    Measured from the container's centre, along x as a share of half the length and along y
    as a share of half the width; what lies beyond a limit counts in whole percent, rounded up.
    A container without the limits costs nothing."""

    name = "balance_limit_cost"
    label_en = "Centre of gravity limit"
    label_ko = "무게중심 한도"
    description_en = (
        "For each container with max_offset_length or max_offset_width, how far its load's "
        "centre of gravity lies from the container's centre beyond those limits: along the "
        "length in percent of half the length, across in percent of half the width, each "
        "rounded up. One unit of cost is one percent beyond a limit; 0 means every load's "
        "centre of gravity is within its limits (or the containers have none)."
    )
    description_ko = (
        "max_offset_length나 max_offset_width가 있는 컨테이너마다 적재물의 무게중심이 "
        "컨테이너 중심에서 한도를 넘어 떨어진 만큼을 더합니다. 길이 방향은 길이 절반의 백분율, "
        "폭 방향은 폭 절반의 백분율로 재고 각각 올림합니다. 한도를 넘은 1%가 비용 1이며, "
        "0이면 모든 적재물의 무게중심이 한도 안에 있습니다(또는 한도가 없습니다)."
    )


class SegregationCost(Constraint):
    """Items a segregation rule keeps apart that are not. Default: hard, weight 1.

    Each pair of placed items in one container whose `segregation_class`es a rule of the
    problem's `segregation_rules` keeps out of one container counts 1, and so does each pair
    whose classes must keep a `distance` and stand closer than it in the floor plane."""

    name = "segregation_cost"
    label_en = "Segregation"
    label_ko = "혼적 금지"
    description_en = (
        "The pairs of items in one container whose segregation classes may not share a "
        "container, plus the pairs whose classes must keep a distance and stand closer than it "
        "(the larger of the gaps between their footprints along the length and across). One "
        "unit of cost is one such pair; 0 means every segregation rule holds."
    )
    description_ko = (
        "한 컨테이너에 함께 실으면 안 되는 혼적 금지 등급(segregation_class)의 물품 쌍과, "
        "일정 거리를 두어야 하는데 그보다 가까이 놓인 물품 쌍(길이 방향과 폭 방향으로 바닥 면 "
        "사이 간격 중 큰 값)을 셉니다. 한 쌍이 비용 1이며, 0이면 모든 혼적 금지 규칙이 "
        "지켜졌습니다."
    )


class AxleLoadCost(Constraint):
    """Axle groups loaded above their `max_load`. Default: hard, weight 1.

    For a container with two `axle_groups`, the load on each follows from the lever rule: a
    placed item of weight w whose middle is at x puts w (rear - x) / (rear - front) on the
    front group, the rest on the rear, plus each group's `empty_load` or share of
    `tare_weight`. Checked at departure and after each stop's items are unloaded; the worst
    stage counts, each group's excess rounded up to a whole weight unit and the two added."""

    name = "axle_load_cost"
    label_en = "Axle loads"
    label_ko = "축중"
    description_en = (
        "For each container with axle groups, the load each group carries above its max_load, "
        "by the lever rule over the placed items (plus the empty loads or the tare), at the worst "
        "stage of the route: at departure or after a stop's items are unloaded. One unit of cost "
        "is one weight unit over a limit; 0 means no axle group is overloaded at any stage."
    )
    description_ko = (
        "축 그룹이 있는 컨테이너마다, 놓인 물품(과 공차 하중 또는 tare)에 지렛대 법칙을 적용해 "
        "각 축 그룹이 받는 하중이 max_load를 넘는 만큼을 출발 시점과 각 정차지에서 물품을 내린 "
        "뒤 중 가장 나쁜 단계에서 더합니다. 한도를 넘은 중량 1단위가 비용 1이며, 0이면 어떤 "
        "단계에서도 축이 과적되지 않았습니다."
    )


class UsedContainerCost(Constraint):
    """Cost of the containers in use. Default: soft, weight 1.

    A container is in use when it holds a placed item; it then costs its `cost` (1 by
    default, so the total is the number of containers used)."""

    name = "used_container_cost"
    label_en = "Containers used"
    label_ko = "사용한 컨테이너"
    description_en = (
        "The sum of the cost of every container that holds a placed item. With every cost at "
        "1 this is the number of containers used; with a cost per carton or vehicle type it is "
        "the price of the ones chosen. 0 only when nothing is placed."
    )
    description_ko = (
        "물품이 하나라도 놓인 컨테이너의 cost를 모두 더합니다. cost가 전부 1이면 사용한 컨테이너 "
        "수이고, 상자나 차량 종류별로 cost를 주면 선택한 것들의 가격입니다. 놓인 물품이 없을 "
        "때만 0입니다."
    )


class OptionalItemCost(Constraint):
    """Optional items left out. Default: soft, weight 1.

    An item with `optional=True` is loaded where there is room; left out, it counts its
    `priority` here (and nothing in `UnplacedItemCost`)."""

    name = "optional_item_cost"
    label_en = "Optional items left out"
    label_ko = "싣지 못한 선택 물품"
    description_en = (
        "For each container, the priorities of the optional items of its loading order that "
        "found no place (1 each by default). One unit of cost is one optional item of priority 1 "
        "left out; 0 means every optional item is loaded (or there is none)."
    )
    description_ko = (
        "컨테이너마다 적재 순서에 있지만 자리를 찾지 못한 선택 물품(optional)의 priority를 "
        "더합니다(기본 1). priority 1인 선택 물품 하나가 빠지면 비용 1이며, 0이면 모든 선택 "
        "물품이 실렸습니다(또는 선택 물품이 없습니다)."
    )


class UsedLengthCost(Constraint):
    """How far each load reaches from the closed end. Default: soft, weight 0 (opt-in).

    The loading metres of a cargo space, in the caller's length unit. Give it a weight
    to pack towards the closed end."""

    name = "used_length_cost"
    label_en = "Used length"
    label_ko = "사용 길이"
    description_en = (
        "For each container, how far its load reaches from the closed end along the length "
        "(the largest x + placed length), in the length unit of the input. One unit of cost is "
        "one length unit; 0 for an empty container. Off until it is given a weight."
    )
    description_ko = (
        "컨테이너마다 적재물이 안쪽 끝에서 길이 방향으로 얼마나 뻗어 있는지(x + 놓인 길이의 "
        "최댓값)를 입력의 길이 단위로 더합니다. 길이 1단위가 비용 1이며, 빈 컨테이너는 0입니다. "
        "가중치를 주기 전에는 꺼져 있습니다."
    )


class LoadBalanceCost(Constraint):
    """How far each load's centre of gravity is from the centre. Default: soft, weight 0 (opt-in).

    Measured in the x-y plane, in thousandths of the container's half-diagonal: 0 is
    centred, 1000 a corner."""

    name = "load_balance_cost"
    label_en = "Load balance"
    label_ko = "적재 균형"
    description_en = (
        "For each container, the distance of its load's centre of gravity from the container's "
        "centre in the floor plane, in thousandths of the half-diagonal (0 = centred, 1000 = in "
        "a corner). 0 for a container whose placed items weigh nothing. Off until it is given a "
        "weight."
    )
    description_ko = (
        "컨테이너마다 적재물의 무게중심이 바닥 평면에서 컨테이너 중심으로부터 떨어진 거리를 "
        "반대각선의 천분율로 더합니다(0 = 중심, 1000 = 모서리). 놓인 물품의 무게가 0이면 "
        "0입니다. 가중치를 주기 전에는 꺼져 있습니다."
    )


class GroupSplitCost(Constraint):
    """Groups spread over several containers. Default: soft, weight 1.

    Items with the same `group` (the items of an order) belong in one container. A global
    constraint: a single total, not a cost per container."""

    name = "group_split_cost"
    label_en = "Split groups"
    label_ko = "나뉜 그룹"
    description_en = (
        "For each group, the number of containers that hold one of its items beyond the first. "
        "A group kept in one container costs 0, one spread over three containers costs 2. "
        "0 means every group is together (or no item has a group)."
    )
    description_ko = (
        "그룹마다 그 그룹의 물품이 들어 있는 컨테이너 수에서 1을 뺀 값을 더합니다. 한 컨테이너에 "
        "모인 그룹은 0, 컨테이너 세 개에 나뉜 그룹은 2입니다. 0이면 모든 그룹이 한곳에 모여 "
        "있습니다(또는 그룹이 지정된 물품이 없습니다)."
    )


class IncompleteGroupCost(Constraint):
    """Groups loaded in part although their rule asks for all or none. Default: hard, weight 1.

    For each group whose `group_rules` entry sets `all_or_none`, the smaller of its placed
    items and the items left out: what it takes to load it whole or not at all. Use it on
    groups of `optional` items: a group of required items is already all through
    `UnplacedItemCost`, and taking one more of them out trades one cost for the other. A global
    constraint: a single total."""

    name = "incomplete_group_cost"
    label_en = "Groups loaded in part"
    label_ko = "일부만 실린 그룹"
    description_en = (
        "For each group whose rule asks for all or none, the smaller of the number of its items "
        "that are placed and the number that are left out. A group of five with three placed "
        "costs 2; 0 means every such group is loaded whole or not at all."
    )
    description_ko = (
        "전량 아니면 제외(all_or_none) 규칙이 있는 그룹마다, 실린 물품 수와 빠진 물품 수 중 "
        "작은 값을 더합니다. 다섯 개 중 세 개가 실린 그룹은 2입니다. 0이면 그런 그룹이 모두 "
        "전부 실렸거나 하나도 실리지 않았습니다."
    )


class SingleContainerCost(Constraint):
    """Groups that must ship in one container spread over several. Default: hard, weight 1.

    For each group whose `group_rules` entry sets `single_container`, the containers holding a
    placed item of the group beyond the first. A global constraint: a single total."""

    name = "single_container_cost"
    label_en = "Groups in one container"
    label_ko = "한 컨테이너에 실을 그룹"
    description_en = (
        "For each group whose rule asks for one container, the number of containers that hold a "
        "placed item of the group, beyond the first. Items left out do not count. 0 means every "
        "such group stands in one container."
    )
    description_ko = (
        "한 컨테이너(single_container) 규칙이 있는 그룹마다, 그 그룹의 물품이 놓인 컨테이너 수에서 "
        "1을 뺀 값을 더합니다. 빠진 물품은 세지 않습니다. 0이면 그런 그룹이 모두 한 컨테이너에 "
        "있습니다."
    )


#: Ready-made default instances (``unplaced_item_cost`` is ``UnplacedItemCost()``).
unplaced_item_cost = UnplacedItemCost()
container_weight_cost = ContainerWeightCost()
stacking_load_cost = StackingLoadCost()
stacking_rule_cost = StackingRuleCost()
unloading_order_cost = UnloadingOrderCost()
balance_limit_cost = BalanceLimitCost()
segregation_cost = SegregationCost()
axle_load_cost = AxleLoadCost()
used_container_cost = UsedContainerCost()
optional_item_cost = OptionalItemCost()
used_length_cost = UsedLengthCost()
load_balance_cost = LoadBalanceCost()
group_split_cost = GroupSplitCost()
incomplete_group_cost = IncompleteGroupCost()
single_container_cost = SingleContainerCost()

# The default set, in list_constraints() order (the unit constraints, then the globals).
_DEFAULTS = (
    unplaced_item_cost,
    container_weight_cost,
    stacking_load_cost,
    stacking_rule_cost,
    unloading_order_cost,
    balance_limit_cost,
    segregation_cost,
    axle_load_cost,
    used_container_cost,
    optional_item_cost,
    used_length_cost,
    load_balance_cost,
    group_split_cost,
    incomplete_group_cost,
    single_container_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all fifteen default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([UnplacedItemCost(), UsedContainerCost(weight=10)])

    Same contract as the C++ applyConstraints: an empty list is an error (a
    filter that came out empty must not silently flip to "score everything");
    a constraint named several times is scored once per pick, each with its
    own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `cl.constraint_costs(problem)` returns: per constraint, the
    **raw** cost of each container (before weight, exponent, offset and level
    rounding), or the single total of `GroupSplitCost`. Fetch it once and rescore
    with different weights without re-solving::

        raw = cl.constraint_costs(problem)
        score(raw)                     # default set
        score(raw, [UnplacedItemCost(), UsedContainerCost(weight=10)])

    No solver and no search are involved. The arithmetic matches the engine —
    each container contributes ``weight * shape(cost, exponent, offset, clip)`` and
    hard contributions are **rounded per unit** before summing (the same order as
    the engine's applyRaw; rounding the total afterwards gives different values).
    A second pick of a constraint reads the same raw costs with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    return score_costs(costs, get_constraints(constraints))
