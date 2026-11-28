# optimaru.container_loading (Python)

The pybind11 package of the container-loading C++ use case
(`optimaru/container_loading/cpp`). Loads boxes into containers in three dimensions:
every container's `items`, the item indices in loading order, is the only decision
variable, and a placement decoder derives where each item stands from it.

Score, at the default weights:

- hard = -(items that found no place, per container: `unplaced_item_cost`
  + weight above a container's `max_weight`: `container_weight_cost`
  + weight resting on an item above its `max_load_on_top`: `stacking_load_cost`
  + pairs standing on each other against `stacking_rules`, and layers above an item's
    `max_stack_layers`: `stacking_rule_cost`
  + pairs where an item that leaves later blocks one that leaves earlier: `unloading_order_cost`
  + thousandths a load's centre of gravity lies beyond `max_offset_length` / `max_offset_width`:
    `balance_limit_cost`)
- soft = -(the `cost` of the containers in use: `used_container_cost`
  + containers a group is in beyond its first: `group_split_cost`)

`used_length_cost` and `load_balance_cost` are in the catalog at weight 0 and score once a
selection gives them a weight. The problem carries no weights. Pick, re-weight or flip
constraints with `optimaru.container_loading.constraints`, and read the defaults with
`cl.constraint_info()`:

```python
from optimaru import container_loading as cl
from optimaru.container_loading.constraints import GroupSplitCost, UsedLengthCost, get_constraints, score

cl.constraint_info()
# [ConstraintInfo(name='unplaced_item_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...]
problem = cl.generate_demo_data()
picks = [UsedLengthCost(weight=0.001) if spec.name == "used_length_cost" else spec for spec in get_constraints()]
cl.solve(problem, time_limit_seconds=1.0, constraints=picks)
raw = cl.constraint_costs(problem)   # raw per-container costs (group_split_cost: [total])
score(raw), score(raw, picks)
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping (clip=True: only the splits beyond the first count).
score(raw, [GroupSplitCost(), GroupSplitCost(weight=100, offset=1)])
```

`weight` (None = the default), `level` (None = the default level, otherwise "hard" or
"soft", or `Level.HARD` / `Level.SOFT`; the domain has no medium level, so "medium" raises
`ValueError`), `exponent` (1.0) and the keyword-only `offset` (0.0) / `clip` (True) are the
knobs. The exponent shapes each container's cost for the nine per-container constraints
and the one total of `group_split_cost`. `offset` / `clip` shift that shaping at the same
granularity: `clip=True` counts only what lies beyond the offset, `clip=False` shapes
`cost - offset` as it is, on every container the constraint evaluates, an empty one
included (`stacking_rule_cost` evaluates every container only when an item can break a
rule or a layer limit, `balance_limit_cost` only the containers with a limit;
`constraint_costs()` lists them as `units`). That is refused on
hard, where hard 0 must mean no violation: `ValueError: constraint 'container_weight_cost':
clip=False with a non-zero offset cannot be hard, because hard 0 must mean no violation;
use clip=True (a threshold) or another level`. On soft it is allowed.

## Constraints

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
| --- | --- | --- | --- | --- |
| `unplaced_item_cost` | `UnplacedItemCost` | hard | 1 | per container: the items of its list the decoder could not place |
| `container_weight_cost` | `ContainerWeightCost` | hard | 1 | per container: the weight of its list above `max_weight` |
| `stacking_load_cost` | `StackingLoadCost` | hard | 1 | per container: the weight resting on each item above its `max_load_on_top`, rounded up per item |
| `stacking_rule_cost` | `StackingRuleCost` | hard | 1 | per container: the pairs where an item stands on another although a stacking rule forbids its `stack_group` on the other's, plus the layers above each item's `max_stack_layers` |
| `unloading_order_cost` | `UnloadingOrderCost` | hard | 1 | per container: the pairs where an item with a higher `unload_order` stands between one with a lower order and the door, or above it |
| `balance_limit_cost` | `BalanceLimitCost` | hard | 1 | per container: the thousandths of half its length (width) its load's centre of gravity lies beyond `max_offset_length` (`max_offset_width`), each rounded up |
| `used_container_cost` | `UsedContainerCost` | soft | 1 | per container: its `cost` when it holds a placed item |
| `used_length_cost` | `UsedLengthCost` | soft | 0 (opt-in) | per container: the largest `x + placed_length` |
| `load_balance_cost` | `LoadBalanceCost` | soft | 0 (opt-in) | per container: the centre of gravity's distance from the centre of the floor, in thousandths of the half-diagonal |
| `group_split_cost` | `GroupSplitCost` (global) | soft | 1 | over the groups: the containers that hold one of its items, beyond the first |

All ten are flippable and none is a reward; the [guide](README.md#constraints) defines
each raw cost exactly.

## Install and test

```bash
OPTIMARU_USE_CASES=container_loading uv sync   # builds only this use case's C++ extension
uv run pytest -q tests/container_loading
```

`tests/container_loading/reference_container_loading.py` is an independent pure-Python
check of a solved model: that its positions are a loading, and what every constraint
costs on them. `tests/container_loading/test_reference_parity.py` holds the C++ domain to
it.

## Usage

```python
from optimaru import container_loading as cl

problem = cl.ContainerLoadingProblem(
    containers=[cl.Container("van", 3000, 1700, 1800, max_weight=1_200_000)],
    items=[
        cl.Item("stop-2-pallet", 1200, 800, 900, weight=250_000, rotations=cl.Rotation.UPRIGHT, unload_order=2),
        cl.Item("stop-1-glass", 600, 400, 300, weight=8_000, rotations=cl.Rotation.UPRIGHT,
                max_load_on_top=0, unload_order=1),
        cl.Item("stop-1-carton", 600, 400, 400, weight=15_000, unload_order=1),
    ],
)
cl.solve(problem, time_limit_seconds=1.0, random_seed=0)   # in-place solve
print(problem.score)                                       # 0hard/-1soft
for item in problem.items:
    print(item.id, item.sequence, (item.x, item.y, item.z))
print(problem.loading(), problem.containers[0].used_length, problem.containers[0].fill_ratio)
```

The derived fields (`Item.container`, `sequence`, `x`, `y`, `z`, `placed_length`,
`placed_width`, `placed_height`, `is_placed()`; `Container.placed_items`,
`unplaced_items`, `loaded_weight`, `loaded_volume`, `used_length`, `fill_ratio`;
`problem.containers_used()`, `problem.unplaced_items()`) are written by `calculate_score`
and by every solve. `problem.loading()` is the decision, one list of item indices per
container; `problem.set_loading([...])` writes it.

For finer control build `cl.SolverConfig()` and set attributes (time budget
`stop.time_limit_seconds`, `stop.step_limit`, `run.parallel_solver_count`, the ALNS knobs,
the move mix `moves.relocate_weight` / `list_swap_weight` / `two_opt_weight` /
`or_opt_weight`, the construction `construction.first_fit_decreasing`, the domain's moves
`custom_moves.empty_container` / `custom_moves.empty_container_candidates`,
`custom_moves.repack`, `custom_moves.switch_container` and `custom_moves.stop_order`,
nearby selection by stop `selection.nearby_probability` / `selection.nearby_size` (see the
guide's [Search](README.md#search)),
`stop.target_score`, the `on_new_best(step, score)` callback, `full_assert`, ...), then
call `cl.Solver(cfg).solve(problem)` or `cl.solve(problem, config=cfg)`. Zero budget is
`stop.time_limit_seconds = 0`, a fixed number of steps is `stop.step_limit`.

Note: `problem.items[i]` and `problem.containers[i]` return copies. Write loading orders
in bulk with `problem.set_loading([...])`, or assign a whole list back
(`problem.containers = containers`).

## JSON in, JSON out

The JSON contract is `optimaru.container_loading.models`; the
[guide](README.md#the-json-model) has every field.

```python
from optimaru.container_loading.models import ContainerLoadingProblemModel

model = ContainerLoadingProblemModel.model_validate(data)    # JSON -> pydantic
solved = cl.solve(model, time_limit_seconds=5.0)             # a new model, with the outputs
print(solved.score_hard, solved.containers_used, solved.unplaced_items)
print(solved.model_dump_json(exclude_none=True))
```

`solve` given a model converts it, solves, and returns a new model; the one passed in is
left as it was. `model.to_core()` and `ContainerLoadingProblemModel.from_core(problem)`
are the conversions: `to_core()` loads each container's `items` (and `unplaced_items`)
when a solve has filled them in, else its `initial_items`; `from_core()` decodes the
problem first (`cl.update_placements`), so the positions describe the loading orders the
problem holds, and reports `problem.score` as a solve or `calculate_score` last set it.

Two ways to go on from a solved model start from different places. `cl.solve(solved)`
loads each container's `items` and then its `unplaced_items`, so the items a solve gave to
a container stay with it and are tried again there. `with_initial_solution(problem,
solved)` copies only the placed `items` into `initial_items`, so the unplaced items are
constructed afresh and may go to any container. Use the first to go on with the same
solve; use the second to carry a loading over to a problem whose containers or items
changed, or to let the unplaced items look for another container.

`solve_request` takes a pydantic request, `models.ContainerLoadingRequest`: the problem
model as `model_input`, with the settings of the solve under the names of `solve`'s
keywords (`time_limit_seconds`, `random_seed`, `parallel_solver_count`,
`stop_at_first_feasible`, `full_assert`, and `solver_parameters` for further
`SolverConfig` settings by group, such as `{"custom_moves": {"empty_container": false}}`).
It returns the solved `ContainerLoadingProblemModel`, as `solve(model, ...)` does, and
leaves the request unchanged. `constraints=` is the selection `solve` takes.

```python
from optimaru.container_loading import solve_request
from optimaru.container_loading.models import ContainerLoadingRequest

request = ContainerLoadingRequest.model_validate_json(text)  # or ContainerLoadingRequest(model_input=model, time_limit_seconds=5.0)
solved = solve_request(request)
```

With an API key set (`optimaru.use_api`, `OPTIMARU_API_KEY`), `solve(model)` and
`solve_request` run on the optimaru API and `solve(problem)` on a compiled problem is
refused: it is solved in place, which a result that comes back as JSON cannot do. See
[Solving on the optimaru API](../README.md#solving-on-the-optimaru-api).

## Parameters and their costs

A model that sets a field which acts only through a cost, while its constraints leave
every such cost out, is refused with `ValueError` naming the field and the cost to add;
a pick at weight 0 counts. Here: a container's `max_weight` (`container_weight_cost`) and
`max_offset_length` / `max_offset_width` (`balance_limit_cost`), an item's
`max_load_on_top` (`stacking_load_cost`), `unload_order` (`unloading_order_cost`), `group`
(`group_split_cost`) and `max_stack_layers` / `stack_group` (`stacking_rule_cost`), and the
problem's `stacking_rules` (`stacking_rule_cost`). `bottom_only` and `max_stack_height`
are the decoder's and need no cost.
`models.parameter_uses(model)` lists the fields an input sets, and
[Parameters and their costs](../README.md#parameters-and-their-costs) has the table.
It is checked where the input is the JSON model (`solve_request`, `solve(model)`); a
compiled problem is solved as given.
