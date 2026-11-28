# Container loading

Three-dimensional loading of boxes into containers, on this repository's C++ engine
(`core/optimaru/`): boxes onto pallets and into rack bays, a route's parcels into a
vehicle's cargo space in the order of its stops, an order's items into the cheapest
shipping cartons of a catalog. Original to optimaru; the placement rule is the extreme
points of Crainic, Perboli and Tadei (2008).

The decision is an **order**, not a set of coordinates: every container has a list of
items, its loading order, and a decoder places the items one after the other. Positions
are outputs.

Python API details: [`python.md`](python.md).

## Usage

```sh
OPTIMARU_USE_CASES=container_loading uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/container_loading
```

```python
from optimaru import container_loading as cl

problem = cl.ContainerLoadingProblem(
    containers=[cl.Container(f"pallet-{c + 1}", 1200, 800, 1500, max_weight=600_000) for c in range(3)],
    items=[cl.Item(f"carton-{i + 1}", 400, 300, 300, weight=6000) for i in range(60)],
)
print(cl.solve(problem, time_limit_seconds=2.0, random_seed=0).score)   # 0hard/-2soft
print(problem.containers_used(), problem.unplaced_items())              # 2 []
for item in problem.items[:3]:
    print(item.id, problem.containers[item.container].id, (item.x, item.y, item.z),
          (item.placed_length, item.placed_width, item.placed_height))
```

`solve` returns the problem it was given, solved in place; the score is its `score`
(`.hard`, `.soft`). It takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
`stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config`
(`container_loading.SolverConfig`) and `constraints`:

```python
from optimaru.container_loading.constraints import UsedLengthCost, get_constraints, score

cl.constraint_info()                    # the catalog: name, default level / weight, flags
picks = [UsedLengthCost(weight=0.001) if spec.name == "used_length_cost" else spec for spec in get_constraints()]
cl.solve(problem, time_limit_seconds=1.0, constraints=picks)
raw = cl.constraint_costs(problem)      # raw per-container costs, re-priced in pure Python
score(raw), score(raw, picks)
```

`cl.generate_demo_data()` builds the demo: 40 boxes of six kinds (one fragile) for four
pallets, for three stores (the groups).

## The JSON model

`optimaru.container_loading.models` is the contract of the optimaru API for this use case
(slug `container-loading`): a request in, the same problem with its outputs filled in out.

Lengths and weights are whole numbers in whatever unit the caller uses; the examples here
use millimetres and grams. Coordinates: `x` runs along a container's `length`, from the
closed end (0) to the door (`x = length`); `y` along its `width`; `z` up. An item's
`x, y, z` is its corner nearest the origin.

**`ContainerLoadingRequest`**: `model_input` (the problem below) and the settings every
request has (`time_limit_seconds`, `random_seed`, `parallel_solver_count`,
`stop_at_first_feasible`, `full_assert`, `solver_parameters`; see
[One way to solve](../README.md#one-way-to-solve)).

**`ContainerLoadingProblemModel`**

| Field | Type, default | Meaning |
| --- | --- | --- |
| `containers` | list of `ContainerModel`, at least one | the spaces that may be used |
| `items` | list of `ItemModel` | the boxes |
| `min_support_ratio` | float in (0, 1], 0.8 | share of an item's base that must rest on the floor or on other items |
| `group_rules` | list of `GroupRuleModel`, [] | how the items of a group are loaded: `{"id": "order-17", "all_or_none": true, "single_container": true}` = every item whose `group` is `order-17` or none of them, and all in one container; `id` must be some item's `group`, each once |
| `stacking_rules` | list of `StackingRuleModel`, [] | pairs of stack groups: `{"top": "chemicals", "bottom": "food"}` = an item whose `stack_group` is `chemicals` may not stand on one whose `stack_group` is `food`; anything not listed may stand on anything |
| `score_hard`, `score_soft` | output | the score (hard 0 = feasible) |
| `containers_used` | output, int | containers that hold a placed item |
| `unplaced_items` | output, list of ids | items that are not placed |

**`ContainerModel`**: one physical space: a pallet, a rack bay, a cargo space, one carton.

| Field | Type, default | Meaning |
| --- | --- | --- |
| `id` | str | unique |
| `length`, `width`, `height` | int, 1 to 1,000,000 | inner size |
| `max_weight` | int or None, None | weight it may hold; None = no limit |
| `cost` | float >= 0, 1.0 | what using this container costs (1 = count containers) |
| `max_stack_height` | int or None, None | how high the load may reach (a hand's height): the decoder fills up to the lower of it and `height`; None = up to `height` |
| `max_offset_length`, `max_offset_width` | float in [0, 1] or None, None | how far the placed load's centre of gravity may lie from the container's centre, as a share of half the length (width); None = no limit |
| `initial_items` | list of item ids, [] | warm start: the loading order to start from |
| `items` | output, list of ids | the placed items, in loading order |
| `unplaced_items` | output, list of ids | items the solve gave to this container that found no place in it |
| `loaded_weight`, `loaded_volume` | output, int | weight and volume of the placed items (`container_weight_cost` counts the unplaced ones too) |
| `fill_ratio` | output, float | `loaded_volume` / the container's volume |
| `used_length` | output, int | largest `x + placed_length` of a placed item (0 when empty) |

**`ItemModel`**

| Field | Type, default | Meaning |
| --- | --- | --- |
| `id` | str | unique |
| `length`, `width`, `height` | int, 1 to 1,000,000 | size as given, before any rotation |
| `weight` | int, 0 to 1,000,000,000, 0 | |
| `rotations` | `"all"`, `"upright"` or `"fixed"`, `"all"` | all = the six axis-parallel orientations; upright = the height stays vertical (two); fixed = as given |
| `max_load_on_top` | int or None, None | weight this item can carry; 0 = nothing on top; None = no limit |
| `unload_order` | int or None, None | lower leaves first (a route's stop number); None = no order |
| `group` | non-empty str or None, None | items of one group belong in one container (the items of an order) |
| `bottom_only` | bool, false | only on the floor (`z = 0`); other items may still stand on it |
| `max_stack_layers` | int >= 1 or None, None | at most this many items in a column through this item, itself included (see [Constraints](#constraints)); None = no limit |
| `stack_group` | non-empty str or None, None | the group the problem's `stacking_rules` name |
| `priority` | int, 1 to 1,000,000, 1 | what leaving it out costs: an unplaced item counts its priority |
| `optional` | bool, false | loaded where there is room: left out, it costs `optional_item_cost` (soft) instead of `unplaced_item_cost`, and its weight does not count against `max_weight` |
| `container` | output, id or None | the container it stands in; None = not placed |
| `sequence` | output, int | 0-based place in its container's loading order |
| `x`, `y`, `z` | output, int | its corner nearest the origin |
| `placed_length`, `placed_width`, `placed_height` | output, int | size along x, y and z after rotation |

An item that is not placed has `container: null` and no position, is listed in the
problem's `unplaced_items`, and counts in `score_hard`.

A request and what comes back, shortened
([`tests/container_loading/fixtures/van_route.json`](../../../tests/container_loading/fixtures/van_route.json)
is the whole problem: ten parcels for three stops in one van):

```json
{
  "time_limit_seconds": 5,
  "model_input": {
    "containers": [{"id": "van-1", "length": 3000, "width": 1700, "height": 1800, "max_weight": 1200000}],
    "items": [
      {"id": "stop1-glass", "length": 600, "width": 400, "height": 300, "weight": 8000,
       "rotations": "upright", "max_load_on_top": 0, "unload_order": 1, "group": "stop-1"},
      {"id": "stop3-pallet", "length": 1200, "width": 800, "height": 1000, "weight": 300000,
       "rotations": "upright", "max_load_on_top": 50000, "unload_order": 3, "group": "stop-3"}
    ]
  }
}
```

```json
{
  "containers": [{"id": "van-1", "items": ["stop3-pallet", "stop1-glass"], "unplaced_items": [],
                  "loaded_weight": 308000, "loaded_volume": 1032000000, "fill_ratio": 0.1124,
                  "used_length": 800, "...": "the inputs, unchanged"}],
  "items": [
    {"id": "stop1-glass", "container": "van-1", "sequence": 1, "x": 0, "y": 1200, "z": 0,
     "placed_length": 600, "placed_width": 400, "placed_height": 300, "...": "the inputs"},
    {"id": "stop3-pallet", "container": "van-1", "sequence": 0, "x": 0, "y": 0, "z": 0,
     "placed_length": 800, "placed_width": 1200, "placed_height": 1000, "...": "the inputs"}
  ],
  "score_hard": 0, "score_soft": -1.0, "containers_used": 1, "unplaced_items": []
}
```

### Three ways to use it

- **Pallets and rack bays.** One container per pallet or bay, `max_weight` its load
  limit, `max_load_on_top` on what must not be crushed. The solve fills as few as it can.
- **A vehicle's cargo space, in stop order.** One container for the cargo space, the door
  at `x = length`, and each parcel's `unload_order` the number of its stop on the route.
  `unloading_order_cost` then keeps every parcel reachable when its stop comes: nothing
  that leaves later stands between it and the door, or on it. Give `used_length_cost` a
  weight to keep the load at the closed end (loading metres).
- **Stacking rules and balance.** `bottom_only` keeps a heavy or liquid item on the floor,
  `max_stack_layers` caps how many items stand in a column through an item (the "stack no
  more than n high" printed on a carton), `max_stack_height` keeps the load within a hand's
  reach, and `stacking_rules` forbid one kind of item on another (chemicals on food).
  `max_offset_length` and `max_offset_width` put a hard limit on how far a load's centre of
  gravity may lie off the centre, which `load_balance_cost` only prefers.
- **More than fits.** When not everything fits, `priority` says what to leave out first, and
  `optional` marks what may be left out at a soft cost (fillers, next-day goods). A
  `group_rules` entry keeps an order whole or out (`all_or_none`) and in one container
  (`single_container`).
- **Cartons from a catalog.** List one container per carton you are willing to use: for a
  catalog of three sizes and an order that may need up to two cartons, six containers,
  each with its carton's `cost`. `used_container_cost` is then the price of the cartons
  chosen, and the solve returns the cheapest ones that hold the order. Several orders in
  one request: give each order's items a `group`, and `group_split_cost` keeps an order
  in one carton where it fits.

## Domain (sequence shape)

- `Container.items` is the only decision variable: the item indices in loading order.
  Every item is in exactly one container's list once a solve returns.
- The **decoder** turns a container and its list into positions. It keeps the corners an
  item may be put at ("points"; an empty container has one, its origin). The next item of
  the list goes to the first point, in (x, z, y) order (from the closed end towards the
  door, then up, then across), at which one of its allowed orientations is *feasible*:
  inside the container, overlapping no placed item, and with at least
  `min_support_ratio` of its base on the floor or on the tops of placed items. Of the
  orientations feasible there it takes the one that divides the free room best (the most
  axes along which the room up to the next item or wall is a whole multiple of the item's
  size, so equal items line up into full rows and layers); of equals, the lowest, then the
  one that reaches less far towards the door. A placed item covers some points and adds
  up to six: its three far corners, each slid back along the two other axes until it
  meets an item or a wall.
- An item for which no point works is **unplaced**. It stays in its list, changes nothing
  for the items after it, and `unplaced_item_cost` counts it. The search moves it to
  another container or to another place in the order.
- **Optional items and the weight limit.** An `optional` item whose weight would put the
  items placed so far in its container above `max_weight` is left out, wherever there is
  room: the only place where the decoder reads a weight limit.
- **Floor and height.** A `bottom_only` item is tried only at points on the floor, and
  nothing is placed above a container's `max_stack_height`: where there is no such room,
  the item is unplaced.
- **Unloading orders, load limits and stacking rules.** When items set an `unload_order`,
  a `max_load_on_top`, a `max_stack_layers` or a `stack_group` the `stacking_rules` name,
  a place is also judged by what it would break. It *blocks* when the item would stand in
  the way of a placed item that leaves earlier (between it and the door, or above it), or
  have a placed item that leaves later in its own way; it *breaks a stacking limit* when
  the item's weight, passed down through what it stands on, would put more on a placed
  item than its `max_load_on_top`, when it would stand on an item (or have one stand on
  it) against a stacking rule, or when it would make a column taller than its own
  `max_stack_layers` or that of an item under it (columns as the decode builds them: a
  box that ends under another's overhang is not seen carrying it, as for the load
  limits, while the cost counts it). The item goes to the first point where
  an orientation does neither; if there is none, to the first that only blocks; if there
  is none either, to the first feasible point, as without them. So an item is never left
  out for an order or a limit, and a list in stop blocks (what leaves last first) comes
  out with each stop in front of the later ones wherever the room allows. An item with an
  `unload_order` also takes, of the orientations feasible at its point, the shallowest
  along x first (then the one that divides the room best), so that a stop builds a thin
  wall across the space and not a long row. This follows the items' facts, whether or not
  `unloading_order_cost`, `stacking_load_cost` and `stacking_rule_cost` are in the
  selection; a model that sets one of their inputs without its cost is refused anyway
  ([Parameters and their costs](#parameters-and-their-costs)).
- The decode reads the sizes, the rotations, `min_support_ratio`, `max_stack_height`,
  `bottom_only`, the unloading orders, the weights, the load limits, the layer limits and
  the stacking rules, `optional` and, for optional items, the containers' `max_weight`,
  and nothing of the balance limits or the groups. Whatever it does, every constraint is a cost computed on its result. So a hard
  cost of 0 means every one of them holds, and a solve that cannot reach 0 still returns a
  loading, with what is broken counted.

`calculate_score(problem)` decodes every container, scores at the default weights and
writes the derived fields (`update_placements(problem)` writes them without scoring): per item
`container` (an index, -1 = not placed), `sequence`, `x`, `y`, `z` and the placed size; per
container `placed_items`, `unplaced_items`, `loaded_weight`, `loaded_volume`,
`used_length` and `fill_ratio`. Every solve leaves them current.

## Constraints

The score is the engine's `Score` type as it is, with no mapping.

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `unplaced_item_cost` | `UnplacedItemCost` | hard | 1 | per container: the priorities of the items of its list the decoder could not place, optional ones aside (the count while every priority is 1) |
| `container_weight_cost` | `ContainerWeightCost` | hard | 1 | per container: the weight of its list above `max_weight` (0 without a limit) |
| `stacking_load_cost` | `StackingLoadCost` | hard | 1 | per container: over its placed items with `max_load_on_top`, the weight resting on the item above that limit, rounded up per item |
| `stacking_rule_cost` | `StackingRuleCost` | hard | 1 | per container: the pairs (a, b) of placed items where a stands on b and a stacking rule forbids a's `stack_group` on b's, plus, over the placed items with `max_stack_layers`, the layers the tallest column through the item has beyond that limit |
| `unloading_order_cost` | `UnloadingOrderCost` | hard | 1 | per container: the pairs (a, b) of placed items with `unload_order(a) < unload_order(b)` where b blocks a |
| `balance_limit_cost` | `BalanceLimitCost` | hard | 1 | per container: how far the placed items' centre of gravity lies from the container's centre beyond `max_offset_length` (in percent of half the length) plus beyond `max_offset_width` (in percent of half the width), each rounded up to a whole percent |
| `used_container_cost` | `UsedContainerCost` | soft | 1 | per container: its `cost` when it holds a placed item |
| `optional_item_cost` | `OptionalItemCost` | soft | 1 | per container: the priorities of the optional items of its list that found no place |
| `used_length_cost` | `UsedLengthCost` | soft | 0 (opt-in) | per container: `used_length`, the largest `x + placed_length` |
| `load_balance_cost` | `LoadBalanceCost` | soft | 0 (opt-in) | per container: the distance of the load's centre of gravity from the container's centre in the x-y plane, in thousandths of the half-diagonal |
| `group_split_cost` | `GroupSplitCost` (global) | soft | 1 | over the groups: the containers whose list holds one of its items, beyond the first |
| `incomplete_group_cost` | `IncompleteGroupCost` (global) | hard | 1 | over the groups whose rule sets `all_or_none`: the smaller of the items placed and the items left out |
| `single_container_cost` | `SingleContainerCost` (global) | hard | 1 | over the groups whose rule sets `single_container`: the containers holding a placed item of the group, beyond the first |

Every constraint is flippable (`level="hard"` / `level="soft"`, or `Level.HARD` /
`Level.SOFT`; `level="medium"` raises `ValueError`, the domain has no medium level), none
is a reward, and eleven of the thirteen are scored by default (`default_enabled`):
`used_length_cost` and `load_balance_cost` weigh 0 until a selection gives them a weight.
`container_loading.constraint_info()` reports exactly this table from the C++ components
(`kDefaultLevel` / `kDefaultWeight`). The unit constraints come first in
`list_constraints()`, the three globals last. An item in no list contributes nothing.

What the raw costs mean exactly:

- **Stacking load.** An item passes its own weight plus the load on it down to the items
  right under it, shared by contact area; an item on the floor passes it to the floor.
  The load on an item is what arrives from above. Two bases side by side under one box of
  9 kg carry 4.5 kg each. `max_load_on_top=0` means nothing may stand on the item.
- **Unloading order.** b blocks a when it stands between a and the door (at or beyond
  a's far end along x, overlapping a across and in height) or above a (at or above a's
  top, overlapping a in the floor plane). Items without `unload_order` are in nobody's
  way and wait for nobody; items of the same stop do not block each other.
- **Stacking rules.** a *stands on* b when a's base is at b's top and the two overlap in the
  floor plane, however little. A rule `{"top": "chemicals", "bottom": "food"}` counts every
  such pair with a chemicals item on a food item, one each; an item on two forbidden items
  counts two. The direction is the one named: food on chemicals is allowed unless a rule
  says otherwise, and a group may be forbidden on itself. An item without a `stack_group`,
  or with one no rule names, breaks no rule.
- **Layers.** An item's layer is 1 on the floor, else one more than the highest layer of
  the items it stands on. The column through an item is its layer plus the tallest chain of
  items standing on it, one on the other: a box at layer 2 with two more stacked on it is
  in a column of 4. An item with `max_stack_layers` counts the layers of that column beyond
  its limit: 3 boxes in a column, the bottom one allowing 2, cost 1; the bottom one
  allowing 1 cost 2. An item across two others tops both columns.
- **Balance limit.** The centre of gravity is the placed items' weights times their centres
  in the floor plane, as for `load_balance_cost`. Along x its distance from the
  container's centre is taken as a share of half the length, across as a share of half the
  width; what lies beyond `max_offset_length` (`max_offset_width`) counts in percent,
  rounded up (a millionth of a percent over is not counted, so a limit such as 0.3,
  which is not exact in binary, holds where it is met exactly). One item of 400 x 400 in
  the corner of an 800 x 800 floor is 0.5 off along both axes: with `max_offset_length=0.2`
  it costs 30. The decoder loads from the closed end, so a load that reaches only part of
  the length sits towards it whatever the order: an even load over the first half of the
  length is 0.5 off along it. The search can move heavy items further in, not the load
  as a whole; for a part load, give the limit across only, or let `load_balance_cost`
  prefer the centre. One percent costs as much as an unplaced item at the default weights;
  give `BalanceLimitCost` a lower weight where the limit should give way to a box left
  out.
- **Priorities and optional items.** An unplaced item counts its `priority` in
  `unplaced_item_cost`, or in `optional_item_cost` when it is `optional`. Three items of
  priority 1, 1 and 5 for room for two cost 1 when the solve leaves out one of the first
  two. An optional item is left out where it finds no place or, the decoder's rule above,
  where its weight would take the placed load over `max_weight`; an item that fits is
  placed, so the search leaves one out by putting it where there is no room for it.
- **Group rules.** A group's items are *listed* where a container's list holds them and
  *placed* where they found a place. `incomplete_group_cost` counts, for an `all_or_none`
  group, the smaller of the placed and the left-out items: five items with three placed
  cost 2, with none or all placed 0. All or none is a choice only where the items are
  `optional` (others already count in `unplaced_item_cost`, and taking one more of them out
  only trades one hard cost for the other); the group is left out whole
  by listing it where there is no room, which needs a container that is full.
  `single_container_cost` counts, for a `single_container` group, the containers holding
  a placed item of it beyond the first; an item left out splits nothing. `group_split_cost`
  still counts every group's spread by the lists, softly. A rule with both flags false is
  accepted and does nothing.
- **Container weight** counts the whole list, an unplaced item included: what the solve
  gave to a container belongs to it. An unplaced `optional` item is the exception: it is
  out of the load, and its weight with it. `loaded_weight` is the weight of the placed items
  only, so a container whose `loaded_weight` is within `max_weight` can still cost here
  when it has `unplaced_items`. An item too large for every container therefore also
  adds its weight to the list that holds it.
- **Used container.** A container whose list holds only unplaced items is not in use.
- **The hard costs are in different units.** `unplaced_item_cost` counts items (times
  their priority), `incomplete_group_cost` items, `single_container_cost` containers,
  `unloading_order_cost` counts pairs, `stacking_rule_cost` pairs and layers,
  `balance_limit_cost` percent, and `container_weight_cost` and `stacking_load_cost`
  count weight units (grams in the guide's examples). With the
  default weights one gram over a limit weighs as much as one unplaced item, and a 10 kg
  box on an item with `max_load_on_top=0` as much as 10,000 unplaced items. A solve that
  cannot reach hard 0 therefore leaves boxes out rather than overload a container or crush
  one. To trade differently, reweight in the `constraints` of `solve`: e.g.
  `UnplacedItemCost(weight=1000)` makes one unplaced item weigh as much as a kilogram over.
- **Group split** counts where the lists put the items, placed or not.

### Parameters and their costs

`max_weight`, `max_load_on_top`, `unload_order`, `group`, `max_stack_layers`,
`stack_group`, `stacking_rules`, `max_offset_length`, `max_offset_width`, `priority`
(either `unplaced_item_cost` or `optional_item_cost` will do), `optional`
(`optional_item_cost`: without it an optional item left out would count nowhere) and a
group rule's `all_or_none` and `single_container` act only through their cost (a `stack_group` no rule names too: it is refused without
`stacking_rule_cost` all the same).
A model that sets one of them while its constraint selection leaves that cost out is
refused with `ValueError` naming the field and the cost to add; a pick at weight 0
counts. `models.parameter_uses(model)` lists the fields an input sets, and
[Parameters and their costs](../README.md#parameters-and-their-costs) has the table for
every use case. It is checked where the input is the JSON model (`solve_request`,
`solve(model)`); a compiled problem is solved as given. The sizes, `rotations`,
`min_support_ratio`, `bottom_only` and `max_stack_height` are the decoder's and hold under
every selection; `cost` is `used_container_cost`'s own scale.

### Scoring and the Python API

- **No weights on the problem.** `ContainerLoadingProblem` and
  `ContainerLoadingProblemModel` carry no constraint weights (`extra="forbid"`). Weights
  come only from constraint selections; the defaults are the components'.
- **One scoring path.** A solve without `constraints` scores with the default assembly,
  the same components a selection picks from; there is no hand-written incremental
  calculator beside it. `ScoreCalculator` (`cpp/cl/score/score_calculator.hpp`) is the
  full recompute the assembly is checked against, and what scores the solution a default
  solve returns. So `solve(problem)` and `solve(problem, constraints=get_constraints())`
  run the same search and return the same loading.
- **`SolverConfig`** has the engine knobs (`stop.time_limit_seconds`, `stop.step_limit`,
  `unimproved_*`, `stop.target_score`, `run.parallel_solver_count`,
  `acceptance.late_acceptance_size`, the `alns` and `alns_trigger` groups, the move
  weights `moves.relocate_weight`, `moves.list_swap_weight`, `moves.two_opt_weight` and
  `moves.or_opt_weight`, `construction.first_fit_decreasing`,
  `custom_moves.empty_container`, `custom_moves.empty_container_candidates`,
  `custom_moves.repack`, `custom_moves.switch_container`, `on_new_best`, `full_assert`). Zero budget is
  `stop.time_limit_seconds = 0`; a fixed number of steps is `stop.step_limit = N`.
- **Exponent granularity.** The engine shapes a unit constraint per unit and a global on
  its total. The units are containers: the ten per-container constraints are shaped per
  container (two overloads of 3 and 4 at exponent 2 cost 9 + 16, not 49), and the three
  globals shape their one total.
- **`constraint_costs()` shape.** The standard C++-package dict `{name: {costs, weight,
  exponent, offset, clip, global, level, reward, level_flippable, name, label,
  default_weight, default_level}}`. The ten per-container constraints list one cost per
  container, an empty one included; the three globals are `[total]`. Hard costs are
  rounded per container, as the engine does. Three are scoped, and their entries name the
  containers they evaluate as `units`: `stacking_rule_cost` every container when an item
  sets `max_stack_layers` or a `stack_group` a rule names, else none, and
  `balance_limit_cost` the containers with a `max_offset_*`, and `optional_item_cost`
  every container when some item is optional, else none. A problem that uses none of them
  pays nothing for them per move; the two group-rule globals cost nothing per move either
  without a rule that sets a flag.
- **Core checks.** Building, scoring or solving a core problem refuses a
  `min_support_ratio` outside (0, 1], a repeated id, a size outside 1..1,000,000, a weight
  outside 0..1,000,000,000, a negative `max_weight`, `max_load_on_top` or `cost`, a
  `max_stack_layers` outside 1..1,000,000,000, a `max_stack_height` outside 1..1,000,000, a `max_offset_*`
  outside 0..1, a stacking rule with an empty group, a `priority` outside 1..1,000,000, a
  group rule for no item's group or a group ruled twice, an item listed twice and items
  without a container (`ValueError`; the pydantic model refuses
  the same). The bounds keep a volume inside 64 bits.
- **The lists a problem comes with are the starting point.** A solve keeps them and
  constructs only the items in no list; `time_limit_seconds=0` returns the construction.
- **Constraints path.** `solve(..., constraints=...)` is single-start
  (`parallel_solver_count` is ignored there, as in bin packing).
- `problem.items[i]` and `problem.containers[i]` return copies (as in every C++ package):
  write loading orders with `problem.set_loading([...])`; `problem.loading()` reads them.
  `problem.score` is read-only (set by `calculate_score` / `solve`).

### Offsets and repeated picks

Every constraint takes the knobs `weight`, `level`, `exponent` and, keyword-only,
`offset` and `clip`, and may be picked several times. A raw cost `x` (per container for
the ten unit constraints, the one total for each of the three globals) is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which lowers the penalty of
every container below the offset, an empty one included); `offset=0` is the plain power
shaping. `clip=False` with an offset is refused on hard, because hard 0 must mean no
violation. On `stacking_rule_cost`, `balance_limit_cost` and `optional_item_cost`, `clip=False` shifts only the
containers they evaluate (the `units` of their `constraint_costs()` entries). Each pick is its own instance with its own knobs, its level included, and the
instances add up:

```python
from optimaru.container_loading.constraints import UsedLengthCost, get_constraints

cl.solve(problem, constraints=[
    *get_constraints(),                        # the defaults
    UsedLengthCost(weight=0.01, offset=1000),  # + 0.01 per mm a load reaches beyond 1 m
])
```

`cl.constraint_costs(problem, picks)` takes the same picks (the second keyed
`used_length_cost#2`).

## Search

First fit decreasing by decode builds the start; late acceptance then works on the
loading orders with the engine's sequence moves (relocate, swap, Or-opt, 2-opt), its
adaptive ruin-and-recreate kicks, and four moves of the domain
(`optimaru/container_loading/cpp/cl/search/moves/`).

- **Construction** (`construction.first_fit_decreasing`,
  `cpp/cl/construct/construction.hpp`). The items no list holds go in this order: with
  unloading orders, what leaves last first; the `optional` items after the others and the
  higher priorities first, so that what room runs short for is what matters least (this
  ranks above `bottom_only`: a stackable item of a higher priority may take the floor
  first); the `bottom_only` items before the others, so
  that they find the floor free; the items with a `max_load_on_top` after all the others,
  so that they end up on top; then group by group, the group with the most
  volume first; inside a group largest first, by volume, then by base area. Each goes
  into the first container that *takes* it: one whose weight limit holds and whose decode,
  with the new item added, leaves no more items unplaced than before, no more weight on an
  item than it carries, no more items in the way of an earlier stop and no more stacking
  rules or layer limits broken.
  Containers are tried in this order: those that already hold an item of the item's
  group, the other containers in use, then the empty ones, cheapest per volume first. An
  item no container takes goes to the one with the most free volume. It is
  deterministic.
- **`EmptyContainerMove`** (`custom_moves.empty_container`,
  `custom_moves.empty_container_candidates`). `used_container_cost` is a step function: a
  container stops costing only once its last item has left, which no single-item move can
  show the acceptor. This move takes one of the least filled containers in use and moves
  its items, largest first, each to the fullest other container that still takes it (as
  above, and the moved item itself must find a place there). What no other container
  takes stays, so the move may be partial: the score
  stays and the source is nearer to empty.
- **`RepackPairMove`** (`custom_moves.repack`). `group_split_cost` is a step function
  too: a group spread over two containers costs the same until its last item has joined
  the others. This move takes the items out of two containers in use and loads them
  again, into those two and, every second time, an empty one as a third. The items are
  dealt out group by group: a group goes whole to the first of the containers with room
  for all of it (a drawn share of its volume, 70 to 100%, and its weight limit), and a
  group none has room for is dealt item by item. Each container's items are then put in
  the construction's loading order and decoded; what finds no place goes on to the next
  container. The groups the two shared come out whole wherever they fit, and when
  everything fits the first container the second is empty. It is made only when every
  load comes out clean (everything placed, no stacking limit, stacking rule or unloading
  order broken).
- **`SwitchContainerMove`** (`custom_moves.switch_container`; used only when the
  containers differ in size, cost or weight limit). The load of one container goes into
  empty containers of another kind that cost no more: as it is into one, or split, in its
  order, over several opened cheapest first while together they cost no more than the one
  that is left. It is how a load reaches a cheaper carton, or two small ones in place of
  a large one.
- **`StopOrderMove`** (`custom_moves.stop_order`; used only when at least two items set
  an `unload_order`, also for a single container). A vehicle load is one container with
  items for many stops, and the engine's sequence moves know nothing of stops: a relocate
  or a swap mixes the stop blocks, and once mixed a load is rarely put back in order one
  item at a time. This move takes a container whose list holds items with an order, puts
  them back in stop blocks, the latest stop first (items without an order keep their
  places), and makes one change inside one stop's block: an item moved or two swapped,
  or the block ordered afresh by volume, height, base area, depth or at random. A
  container mixed by other moves comes back in blocks; one in blocks tries another way to
  build one stop's wall.
- **Nearby selection by stop** (`selection.nearby_probability`, 0.8, and
  `selection.nearby_size`, 20; used only when at least half the items set an
  `unload_order`, as in a vehicle load). An
  item's nearby items are those of its own stop and the stops next to it, then the
  closest in volume. The move mix relocates and swaps an item next to them most of the
  time, which keeps the blocks, and a ruin-and-recreate kick tries only the places next to
  them where it would otherwise try every place of every list (a decode each).
- **The decode is the cost of a step.** A move changes one or two lists, and each is
  decoded again from the place it changes: item k's place depends on items 0 .. k - 1
  only, so a decode keeps what it needs to go on from any position
  (`cpp/cl/score/packing.hpp`). The constraints share one decode per container and list
  (`PackingCache`), so ten constraints cost one decode, not ten.

### How good, how fast

Where the fewest containers is known: containers of 1200 x 800 x 1000 cut into boxes
(sides in steps of 100), which fit back into as many containers as were cut and into no
fewer. Three seeds per row, one thread of an Apple M4 Max, 10 s per solve; where the
seeds differ they are listed in seed order. "Slack" is the share of the volume taken out
again after the cut; "volume needs" is the number of containers the items' volume fills,
rounded up: the optimum without slack, a lower bound with it
([`container-loading-known-optimum`](../../../benchmarks/container-loading/README.md#where-the-fewest-containers-is-known)):

| cut containers x boxes | slack | rotations | items | volume needs | construction | solve |
| --- | --- | --- | --- | --- | --- | --- |
| 4 x 15 | 0% | fixed | 60 | 4 | 6 (1 unplaced) | 5 |
| 4 x 15 | 0% | all | 60 | 4 | 5 | 5 |
| 4 x 30 | 0% | all | 120 | 4 | 5 | 5 |
| 10 x 20 | 0% | fixed | 200 | 10 | 13/13/12 | 12 |
| 10 x 20 | 0% | all | 200 | 10 | 11/12/11 | 11 |
| 20 x 25 | 0% | all | 500 | 20 | 21 | 21 |
| 4 x 15 | 10% | all | 54/55/53 | 4 | 5 | 4 |
| 10 x 20 | 10% | fixed | 178/178/179 | 10/9/10 | 12/11/12 | 11/11/10 |
| 10 x 20 | 10% | all | 178/178/179 | 10/9/10 | 10 | 10 |
| 20 x 25 | 10% | all | 446/449/450 | 19/19/18 | 20/19/19 | 19 |
| 40 x 50 | 10% | all | 1803/1801/1801 | 36/37/37 | 37 | 37 |

A cut container is full to the last unit, which a real load rarely is, and a placement
rule that never looks ahead leaves gaps: without slack the solve needs one container more
than the fewest on the small instances (5 for 4), one or two more on the middle ones (11
or 12 for 10) and one more on the large one (21 for 20). With a tenth of the volume taken
out again it needs what the volume needs or one more. Most of the result is the
construction's. The search closes a container where a few moves can (every row where
"solve" is below "construction"); on the larger instances it mostly keeps the hard
constraints at 0 and the groups together.

Steps per second on instances of the same kind, one thread (30,000 steps, seed 1, the
median of three solves). A step costs a decode of the lists it touches, so it is the
number of items per container that sets the speed, more than the number of items:

| cut containers x boxes | items | thousand steps per second |
| --- | --- | --- |
| 4 x 15 | 60 | 60 |
| 4 x 30 | 120 | 15 |
| 10 x 20 | 200 | 40 |
| 20 x 25 | 500 | 12 |
| 40 x 50 | 2000 | 3 |

One container holding hundreds of items is the slow case: a decode of a list resumes only
from where the list changes, and each corner the decoder tries is checked against the
boxes near it (the boxes are filed by the height of their tops for the support check, and
along the length once a list has 32 items or more). On two containers of
1200 x 800 x 2000 cut into boxes (10% slack, one spare, 3,000 steps, one run each, the
time including the construction), about 45 items per container ran at 11,600 steps per
second, 90 at 4,500, 180 at 1,500 and 360 at 630 (9,400, 2,800, 770 and 350 before
the decoder filed the boxes). For a trailer of several hundred small parcels, give the
solve time accordingly or group the parcels into larger units first.

**Vehicle loads.** One cargo space, every box's `unload_order` its stop
([`container-loading-vehicle-loads`](../../../benchmarks/container-loading/README.md#vehicle-loads)).
Two kinds of truck: *catalog* trucks carry cartons drawn from nine sizes (a few
pallet-sized crates among them), 85% upright, a tenth with a `max_load_on_top`; whether
every order and limit can be met is not known, but without the orders the engine places
every box of the first four rows. *Cut* trucks are the cargo space cut into one slab per
stop, each slab cut into boxes and half the volume taken out again from the top: the
positions they were cut into meet every order and limit, so hard 0 is reachable. Three
seeds per row, one thread of an Apple M4 Max, the default weights; "pairs" is
`unloading_order_cost`, "unplaced" `unplaced_item_cost`, both per truck, the mean of the
three; before is 0.9.0:

| truck | budget | hard 0 | pairs left | unplaced | first hard 0 (s) |
| --- | --- | --- | --- | --- | --- |
| catalog 2.5t, 12 stops, 120 boxes | 10 s | 0/3 → 1/3 | 338 → 0 | 9.0 → 3.0 | 1.7 |
| catalog 5t, 15 stops, 200 boxes | 10 s | 0/3 → 3/3 | 1003 → 0 | 1.7 → 0 | 2.1/0.3/5.9 |
| catalog 5t, 20 stops, 250 boxes | 30 s | 0/3 → 0/3 | 1401 → 15 | 13.0 → 10.7 | |
| catalog 5t, 25 stops, 220 boxes | 30 s | 0/3 → 0/3 | 1171 → 26 | 4.0 → 11.3 | |
| catalog 5t, 20 stops, 300 boxes | 30 s | 0/3 → 0/3 | 1269 → 151 | 41.7 → 54.7 | |
| cut 2.5t, 10 stops (about 115 boxes) | 10 s | 0/3 → 3/3 | 212 → 0 | 18.3 → 0 | 0.2/0.0/0.3 |
| cut 5t, 15 stops (about 200 boxes) | 30 s | 0/3 → 1/3 | 672 → 186 | 30.3 → 16.3 | 18.4 |
| cut 5t, 20 stops (about 300 boxes) | 30 s | 0/3 → 0/3 | 1729 → 255 | 63.0 → 40.7 | |

With the weights a warehouse system sends (an unplaced item 1,000, an unloading pair 10,
stacking 1 per gram, `load_balance_cost` 1e-4 and `used_length_cost` 1e-6) the same rows
meet every constraint as often (8 of 24 trucks at 30 s, none before). An unplaced item
then costs as much as 100 pairs, so where not everything fits the solve trades pairs
for a box more in the truck, and more pairs are left than at the default weights. Where
hard 0 comes, it comes within seconds: the construction leaves tens to hundreds of pairs, and the
stop-block moves clear them, at several times 0.9.0's speed (on the catalog trucks, 120
boxes ran at 1,340 steps per second where 0.9.0 ran at 480, 200 boxes at 370 for 125,
300 boxes at 215 for 34). What is left is density: a stop's front is ragged, every later stop has to stay
behind it, and on the larger trucks the room runs out before the last stops are in.

Against an exact model: [`benchmarks/container-loading`](../../../benchmarks/container-loading)
compares the engine with a CP-SAT model that places items freely, 30 s each. On the twelve
instances of up to 15 items CP-SAT proves the optimum and the engine reaches it. On the
four of 32 items the engine uses 4, 5, 5 and 4 containers (4 is the fewest) where CP-SAT,
which proves nothing there, ends on 5.

Groups and stacking limits cost containers, and how many is worth knowing before a
result is called good or bad. The demo made three times as large
(`generate_demo_data(item_count=120, container_count=8)`: 120 boxes for three stores, 27
of them fragile, 3.9 pallets of volume), 200,000 steps, seeds 1 to 3, the same on all
three:

| | pallets | store splits |
| --- | --- | --- |
| as it is | 6 | 3 |
| without the stores (`group`) | 5 | |
| without the limits on the fragile boxes (`max_load_on_top`) | 5 | 3 |
| without both | 4 | |

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of a
`ContainerLoadingProblemModel` whose search starts from `solution`, a solved model or the
same thing as a JSON dict. Containers and items are matched by id: each container's
`initial_items` becomes the `items` it has in the solution, replacing what the problem
had, and a container the solution leaves out starts empty. Items the solution placed
nowhere are constructed around the seeded loading orders; with
`time_limit_seconds=0` a complete solution comes back as it went in. An unknown container
or item, or an item listed twice, raises `ValueError`.

```python
from optimaru.container_loading import with_initial_solution
from optimaru.container_loading.models import ContainerLoadingProblemModel

model = ContainerLoadingProblemModel.from_core(cl.generate_demo_data())
solved = cl.solve(model, time_limit_seconds=1.0, random_seed=0)
again = cl.solve(with_initial_solution(model, solved), time_limit_seconds=1.0, random_seed=1)
```

A solved model is itself a starting point: `to_core()` loads the `items` (and
`unplaced_items`) a solve filled in, so `cl.solve(solved, ...)` goes on from there, and
`cl.calculate_score(solved.to_core())` is the score it carries when everything is placed.
An unplaced item does not keep its place in the order: it is loaded back after the placed
ones, which stand where they stood, so such a model can only come back with fewer unplaced
items than the solve reported, though not necessarily with a lower hard score: an item
placed now may stand on a fragile item or in front of an earlier stop. `initial_items` is
read only where a container has no `items` yet.

## C++

- `optimaru/container_loading/cpp/cl/model/domain.hpp`: the domain and the
  `optimaru::SolutionTraits` specialization; `model/demo_data.cpp`: the demo
- `optimaru/container_loading/cpp/cl/score/packing.hpp`: the decoder (`decode`), the
  costs of one decoded container (`unplacedCost`, `optionalCost`, `stackingExcess`,
  `stackingViolations`, `unloadingBlocks`, `balancePerMille`, `balanceExcess`) and
  `PackingCache`
- `optimaru/container_loading/cpp/cl/score/score_calculator.hpp`: `prepare()` (the fact
  checks, the orientations in the decoder's order, the groups and the stack groups the
  rules name), `updateDerived()` and the full recompute
- `optimaru/container_loading/cpp/cl/constraints/`: one component per constraint
  (+ `common.hpp`, which holds the decode the components share);
  `optimaru/container_loading/cpp/cl/constraints.hpp` assembles the default set
  (`defaultConstraints()`)
- `optimaru/container_loading/cpp/cl/construct/construction.hpp`: first fit decreasing by
  decode
- `optimaru/container_loading/cpp/cl/search/moves/`: `empty_container.hpp`,
  `repack_pair.hpp`, `switch_container.hpp` (header-only, so the tests drive them
  directly) and what they share, `list_edit.hpp`
- `optimaru/container_loading/cpp/cl/solver.hpp`: the engine wrapper (construction, late
  acceptance, the three moves, adaptive ruin & recreate, parallel starts)
- `optimaru/container_loading/cpp/main.cpp`: the demo

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=container_loading
cmake --build build -j2
ctest --test-dir build --output-on-failure   # cl_test + cl_plugin_test
./build/container_loading/cl_demo            # 5 s solve; checks hard 0 (exit 0)
```

`cl_test`: the decoder by hand (the order of the points, the support rule, the rotations,
which orientation is taken, an item without a place, `bottomOnly`, the stack height, the
stacking rules and layer limits) and on random lists, a third of them with the stacking
facts (every decode a valid loading, checked without the decoder; a decode resumed from
any shared prefix equal to one from scratch), the costs of a decoded container by hand, `PackingCache`, the fact
checks and derived fields, the construction, the three moves (applied, undone, repeated)
and the solver (a `ScoreChecks::Full` solve, a zero budget, parallel starts).
`cl_plugin_test`: the default assembly against the full recompute on random loadings and
over random moves (on problems with every input in use), raw costs by hand for all thirteen
constraints, the catalog, overrides and
partial assemblies, `constraint_totals()`, every move preview against the applied score,
and `ScoreChecks::Full` solves, with repeated and shifted components too.

`tests/container_loading/reference_container_loading.py` checks a solved model without
the decoder, in plain Python: that the positions are a loading (inside, no overlap, enough
support, an allowed orientation) and what every constraint costs on them, the stacking
loads in exact fractions. `test_reference_parity.py` holds the engine to it on random
loadings and on solves.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **sequence** shape; the domain solver that configures the engine is
`optimaru/container_loading/cpp/cl/solver.hpp` + `optimaru/container_loading/cpp/cl/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt moves
(weights in `Solver::Config`). `EmptyContainerMove`, `RepackPairMove` and
`SwitchContainerMove` are defined in `optimaru/container_loading/cpp/cl/search/moves/` and
registered in `registerMoves()` of `solver.cpp`: use them as the template, and give a new
move a case in `cl_test` (g). A move that needs to know whether an item still finds a
place asks a `PackingCache` of its own, as all three do.

**Changing the construction heuristic.** The domain places every item before the engine
runs (`cpp/cl/construct/construction.cpp`); the engine only fills what is still in no
list, so a different heuristic is a different order or rule there.
`construction.first_fit_decreasing = False` leaves it to the engine's cheapest insertion.

**Changing the placement rule.** The decoder is `decode()` in
`cpp/cl/score/packing.cpp`. Whatever it does, it has to stay a function of the problem's
facts, the container and the list alone, and of the list's prefix (item k's place may
depend on items 0 .. k - 1 only): `cl_test` (b) checks both on random lists.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one
new constraint through this use case's files in six steps: changed from Python, written
as a component, tested, made cheaper per move, turned on by default and measured.

## Benchmark

[`benchmarks/container-loading`](../../../benchmarks/container-loading) compares this use
case with an OR-Tools CP-SAT model that places items freely, on instances whose fewest
containers is known.
