# Changelog

## Unreleased

- **Container loading: axle loads.** Container `tare_weight` and `axle_groups` (two: `id`,
  `position` from the closed end, `max_load`, optional `empty_load`), the loads by the lever rule
  at departure and after each stop's items are unloaded, scored by the new hard constraint
  `axle_load_cost` (the worst stage's excess) and reported per container as `axle_loads`.
- **Container loading: allowed containers and segregation.** Container `tags` and item
  `allowed_containers` (ids or tags; [] = any, the decoder's: an item finds no place elsewhere),
  and item `segregation_class` with the problem's `segregation_rules` (`{"a", "b", "distance"}`:
  not in one container, or at least `distance` apart in the floor plane), scored by the new hard
  constraint `segregation_cost`. The decoder steers away from places closer than a distance; the
  construction tries only the allowed containers.
- **Container loading: priorities, optional items and group rules.** New inputs, off by default:
  item `priority` (1 to 1,000,000, default 1: an unplaced item counts its priority in
  `unplaced_item_cost`), item `optional` (left out at a soft cost, the new `optional_item_cost`,
  its weight out of the load; the decoder leaves it out where it would take the placed weight
  over `max_weight`), and the problem's `group_rules` (`{"id", "all_or_none",
  "single_container"}`), scored by the new hard globals `incomplete_group_cost` and
  `single_container_cost`. The construction loads optional items after the others and higher
  priorities first.
- **Box sizing: board area and shipping cost by billed weight.** Two opt-in soft costs, both at
  weight 0 by default, so every request that was valid before solves exactly as before:
  - `board_area_cost` (`BoardAreaCost`): per box, `quantity x 2 (lw + lh + wh)`, the board of
    the boxes shipped.
  - `shipping_cost` (`ShippingCost`): per box, `quantity x` the price of a shipment's billed
    weight in the rate table of its zone. New input fields: box `dim_divisor` (a parcel in the
    box bills at least `ceil(box volume / dim_divisor)`), parcel `zone`, and the problem's
    `rate_tables` (one per zone: `zone`, and `breaks` of `max_weight` and `price`). The
    parameter check asks for `shipping_cost` in the selection when any of them is set.
  - New outputs: `board_area` and `shipping_cost` on the problem, `billed_weight` and
    `shipping_cost` on every box (where weight is billed). Compiled objects: `RateTable`,
    `RateBreak`, `Box.dim_divisor`, `Parcel.zone`, `BoxSizingProblem.rate_tables`,
    `board_area()`, `billed_weight_per_box()`, `shipping_cost_per_box()`.
  - The construction and the moves price the sizes with the picked weights of both. A shipping
    price depends on the parcel, so with a weight on `shipping_cost` the parcels that pay alike
    in every box form a group with its own order of the boxes.
  - The CP-SAT model of `benchmarks/box-sizing` takes `board_area_weight` and `shipping_weight`.
  - At the default weights the 20 benchmark instances give the published results, and
    step-limited solves take the same steps as before on both paths. On the same instances,
    billed (three zones, ten breaks), the search at 2 s ties CP-SAT's 30 s result (same
    objective, 4 workers) on 10 and 11 of 20 instances for two weightings and is ahead on the
    others; the plan of the least air pays up to 6.8% more freight (1.6% on average) than the
    plan the shipping cost picks. A shipping weight costs 8% to 29% of the steps per second.
- **Documentation by version.** The documentation site has a version menu: `latest/` (the newest
  release), `dev/` (`main`) and one copy per release minor since 0.4, each linking to repository files
  at its own tag. The old addresses redirect to `latest/`.
- **Container loading: stacking rules and a centre of gravity limit.** New inputs, all off by
  default (a request that sets none of them solves exactly as before):
  - item `bottom_only` (only on the floor) and container `max_stack_height` (how high the load
    may reach): the decoder's, no cost needed;
  - item `max_stack_layers` (at most this many items in a column through it), item
    `stack_group` and the problem's `stacking_rules` (`{"top": ..., "bottom": ...}`: the top
    group may not stand on the bottom one), scored by the new hard constraint
    `stacking_rule_cost` (pairs against a rule plus layers above a limit); the decoder steers
    away from places that break them, as it does for load limits;
  - container `max_offset_length` / `max_offset_width` (how far the load's centre of gravity
    may lie from the centre, as a share of half the length or width), scored by the new hard
    constraint `balance_limit_cost` (whole percent of the half size beyond the limit).
  Both constraints are scoped (`constraint_costs()` names their `units`), so a problem that
  does not use them pays nothing for them per move. The construction loads `bottom_only`
  items first, and it, `EmptyContainerMove` and `RepackPairMove` keep the stacking rules as
  they keep the load limits. The CP-SAT comparison refuses instances that set the new inputs.

- **Container loading: the decoder weighs what it breaks, and a stop fills its depth first.**
  - When every place left for an item breaks an unloading order, a load limit or a stacking
    rule, the decoder no longer takes the first one whatever it costs: it weighs the first place
    that only blocks (the pairs it adds), the first that breaks a stacking limit (the weight
    over each `max_load_on_top`, a rule, layer limit or segregation distance, and its pairs) and
    leaving the item out (its `priority` times `unplaced_item_cost`'s weight, or
    `optional_item_cost`'s for an `optional` item), by the weights of the solve's constraints, and takes the cheapest (of equal costs, placing
    wins). At the default weights an item now stays out rather than block two earlier items or
    put any weight on a box that carries no more; with a warehouse system's weights (an
    unplaced item 1,000, a pair 10, 1 per gram) it still blocks up to 100 pairs, or overloads by
    up to 1 kg, rather than stay out. The weights are those of the last solve, kept on the
    problem for `calculate_score` (the defaults on a problem never solved).
  - An item with an `unload_order` prefers, of the places that break nothing, one within the
    depth its stop's items already reach, so a stop fills the cross-section it has started
    before it goes deeper.
  - Faster decoding of vehicle loads and stacking limits: the unloading-order check looks only
    at the boxes of the cells that can block (each cell knows the lowest and highest stop of
    its boxes), and the loads passed down to limited boxes are added as each box is placed
    (and replayed in placement order when a decode resumes) instead of recomputed for every
    box.

  On the generated trucks of `container-loading-vehicle-loads` (one thread, 30 s), the share
  that meets every constraint is unchanged at the default weights (14 of 48 trucks over six
  seeds) and about the same at a warehouse system's (15 → 13 of 48), but what is left is much
  less: at the default weights no stacking excess and at most a few pairs remain (#174 left
  up to 548 pairs per truck), and the unplaced boxes on the 250- and 300-box catalog trucks
  and the 20-stop cut trucks fall from 10.8/48.0/50.7 to 7.0/36.7/17.3 per truck; at the warehouse weights the unplaced boxes there fall by
  40–75%. The 15-stop catalog trucks reach hard 0 within 0.2 s where they took up to 5 s.
  Loads without unloading orders, limits or rules are decoded exactly as before (the
  benchmarks give the same results at the same speed).

- **Container loading: vehicle loads in stop order.** A cargo space with items for many stops
  often came back with hundreds of unloading-order pairs left. Four changes:
  - The decoder judges a place by what it would break when items set `unload_order` or
    `max_load_on_top`: an item goes to the first point where it stands in no earlier stop's way,
    has no later stop in its own way and overloads no item below it (its weight passed down
    through what it stands on); failing that, to the first point that only breaks an order or a
    limit, weighed against leaving the item out (the entry above). An item with an `unload_order` takes the
    shallowest orientation along the length first, so a stop builds a thin wall. Without orders
    and load limits the decoder places every item where it did.
  - A new move, `StopOrderMove` (`custom_moves.stop_order`, on by default when at least two
    items set `unload_order`, also for a single container): a loading order put back in stop blocks, latest
    stop first, with one change inside one stop's block.
  - Nearby selection by stop (`selection.nearby_probability`, 0.8, and `selection.nearby_size`,
    20, only when at least half the items have an unloading order): the move mix moves an item next to items of its own and the
    neighbouring stops, and a ruin-and-recreate kick tries only those places instead of every
    place of the list.
  - A faster decode for every use: the placed boxes are indexed along the length and by the height
    of their tops, and every free corner keeps how far the room reaches from it, so most corners
    are ruled out without a scan.

  On generated trucks of 10 to 25 stops and 110 to 300 boxes (one thread, three seeds per row,
  `container-loading-vehicle-loads`), the trucks that met every order and limit after 30 s went
  from none of 24 to 8 of 24, at the default weights and at the weights a warehouse system sends,
  and the pairs left at the default weights (the mean of three seeds per kind of truck) from
  151–1,729 to 0–255. Loads without unloading orders or stacking limits are decoded exactly as
  before (the benchmarks give the same results), at about the same speed, and twice as fast with
  hundreds of items per container. With stacking limits the decode now also keeps weight off
  them where it can. The guide's "How good, how fast" has the numbers.

## 0.9.0

**From 0.8.0.** Two new use cases, box sizing (`optimaru.box_sizing`, the 23rd) and container
loading (`optimaru.container_loading`, the 24th), and order picking on any warehouse layout (a
grid of any size, or named locations with a distance table). **Breaking:** some public names of
order picking (`OrderPickingSolution` takes a `warehouse`, `calculate_distance` a third argument,
`LocationModel.column` is a string; C++ `op::Warehouse` is an object), several C++ header moves
(`model/` is `solution/`, no umbrella header, `accept/` and `select/` back at the top,
`LocalSearch` is `Solver::run`), and every random draw now goes through the engine's own
functions, so a seeded solve gives a different (equally good) result from 0.8.0's, and the same
draws on every platform.

- **New use case: container loading (`optimaru.container_loading`).** Three-dimensional loading
  of boxes into containers: pallets and rack bays, a vehicle's cargo space in the order of its
  stops, an order's items into the cheapest cartons of a catalog. It is the 24th use case, and the
  first whose decision is not what the answer shows: every container has a loading order (the
  sequence shape), and a placement decoder derives where each item stands from it (extreme
  points; an item goes to the first corner, from the closed end, then up, then across, that takes
  it inside the walls, clear of the others and with `min_support_ratio` of its base supported).
  Eight constraints: `unplaced_item_cost`, `container_weight_cost`, `stacking_load_cost` and
  `unloading_order_cost` (hard), `used_container_cost` and `group_split_cost` (soft), and the
  opt-in `used_length_cost` and `load_balance_cost` (soft, weight 0). Items carry `rotations`
  (`all`, `upright`, `fixed`), `max_load_on_top`, `unload_order` and `group`; containers
  `max_weight` and `cost`. The constraints share one decode per container and list, and a list
  is decoded again only from where it changes. Search: first fit decreasing by decode, the
  sequence moves, `EmptyContainerMove`, `RepackPairMove` and `SwitchContainerMove`
  (`custom_moves.empty_container`, `custom_moves.empty_container_candidates`,
  `custom_moves.repack`, `custom_moves.switch_container`). The JSON model is
  `models.ContainerLoadingRequest` / `ContainerLoadingProblemModel` (API slug
  `container-loading`); the guide is `docs/use-cases/container-loading/`, the notebook
  `examples/container-loading.ipynb`. `benchmarks/container-loading` compares it, on instances
  whose fewest containers is known, with a CP-SAT model that places items freely; that model is
  kept in the benchmark project, and both answers are scored from where the items stand.
- **Engine: one ruin-count helper.** `clamp_ruin_count` in `search/ruin_recreate/ruin_count.hpp`
  now draws the ruin count of the destroy-repair move and the iterated local search;
  `kick_ruin_count` moved there from `kick.hpp`, which still includes it. Seeded results unchanged.
- **New use case: box sizing (`optimaru.box_sizing`), the 23rd.** Which box sizes should a
  warehouse stock? Every parcel (the packed content of one shipment, by its bounding size, with
  the number of shipments that had it) ships in one of the candidate boxes, at most
  `max_box_types` distinct boxes are in use, and the least air is shipped. A parcel fits a box
  when its sorted sides are each at most the box's (any rotation), and when it is not heavier
  than the box's `max_weight`. Six constraints: `parcel_fit_cost`, `box_weight_cost` and
  `box_type_limit_cost` (hard), `void_volume_cost` and `box_cost` (soft), and `box_type_cost`
  (soft, weight 0: picked with a weight, it is the fixed cost of stocking one more size). A box
  can be `required` (already stocked: it counts as in use), a parcel that fits no candidate is
  refused by name, and the solved model carries `box_types_used`, `void_volume`, `fill_ratio`
  and the shipments per box. The search works on the set of sizes: a greedy construction
  (`time_limit_seconds=0` returns it), the moves `CloseBoxMove`, `OpenBoxMove` and
  `SwapBoxMove` (`custom_moves.close_box`, `open_box`, `swap_box`), and late acceptance that
  starts over from just below its best when it stalls (`acceptance.reheat`). On the API the
  use case is `box-sizing`. With it come the guide (`docs/use-cases/box-sizing/`), the notebook
  `examples/box-sizing.ipynb`, and `benchmarks/box-sizing`, whose CP-SAT model lives in that
  directory (`box_sizing_cpsat.model`) and which does not install external-solver.
- **Order picking on any warehouse layout.** The layout was one fixed grid (columns A..E, shelving
  rows 1..3, rows 0..10 on a side); it is now input, in one of two kinds, and a request or solution
  without either solves exactly as before (same seeded results).
  - A grid of any size: `warehouse` on `OrderPickingRequest` (`WarehouseModel`: `column_count` 5,
    `shelving_row_count` 3, `shelving_width` 2, `shelving_height` 10, `shelving_padding` 3, whole
    meters), `op.Warehouse(...)` on the compiled side. A location's `column` is written as
    spreadsheet columns are (`A`..`Z`, `AA`, ... `ZZZ`).
  - Named locations with a walking-distance table, for a building that is no such grid:
    `locations` (ids) and `distances` (`{from: {to: meters}}`) on the request, where the
    `location` of a trolley or an order item is then an id; `op.Warehouse.from_distances(ids,
    table)` and `warehouse.location(id)` on the compiled side (a location belongs to the
    warehouse that made it: a solution of another one refuses it). The table may be asymmetric, a pair
    given one way only is as long the other way, and a pair given neither way is refused. At
    most 2048 locations in a request, 4096 in a compiled `Warehouse`.
  - One request uses one kind; mixing them is refused. `OrderPickingSolution` takes a fourth
    argument, `warehouse`, and has the property of that name; `calculate_distance` takes a third.
    `PickStop.location` is a `LocationModel` or an id, as the request gave it.
  - A solve reads its distances from one location-to-location table for both kinds instead of
    computing each one (a grid: up to 2048 distinct locations in use; parallel starts share
    it), which makes the default
    layout faster: about 1.1x the steps per second on the demo and 1.4x on the demo at four
    times the size, same results.
  - `WarehouseLocation(column, shelving_row, side, row)` no longer checks its numbers against the
    default grid (it cannot know the warehouse): a location outside the grid is refused by
    `OrderPickingSolution(...)`, by `solve` and by what measures (`calculate_score`,
    `distance_to_travel`, `constraint_costs`, `calculate_distance`), with a `ValueError` naming
    the trolley or pick task. `LocationModel.column` is a string of 1 to 3 capital letters instead of a literal
    `A`..`E`, and `shelving_row` and `row` have no upper bound of their own: the request checks
    them against its grid.
  - **Breaking (C++):** `op::Warehouse` is an object (`OrderPickingSolution::warehouse`) and no
    longer a set of constants (`Warehouse::COLUMN_COUNT` is `Warehouse::DEFAULT_COLUMN_COUNT`,
    and so on); `calculateDistance(warehouse, start, end)` measures in a layout, the two-argument
    form in the default grid; `calculateDistanceToTravel` returns `std::int64_t`. New:
    `op/model/layout.hpp` (`makeGridWarehouse`, `makeNamedWarehouse`, `checkLayout`,
    `ActiveDistances`).
  - On the optimaru API, a request that gives no layout is sent as before (it dumps without the
    three new fields). One that gives a layout is refused (422, unknown field) by an API that
    runs an earlier optimaru, until that API follows.
  - The CP-SAT reference (external-solver) knows the default grid only, so the comparison in
    `benchmarks/order-picking` stays on it and another layout is engine-only for now.
- **Breaking (C++): the engine's `model/` is `solution/`.** `core/optimaru/model/` held how the
  engine reads and writes a use case's solution (`SolutionTraits`, `Editor`, `ElementIndex`,
  `SolutionSnapshot`, `Neighborhoods`), not a problem model, so it is
  `core/optimaru/solution/` now: `#include "optimaru/solution/editor.hpp"` and so on, with
  `OPTIMARU_SOLUTION_*` include guards. A use case's own `model/` (its domain and demo data)
  keeps its name. Nothing changes in the Python package or in what the solvers do.
- **Source layout: one file per stop criterion; a use case's own.** `stop/` holds each
  built-in criterion in a file of its own: `max_runtime.hpp` (`MaxRuntime`),
  `no_improvement_time.hpp`, `max_steps.hpp`, `no_improvement_steps.hpp`, `target_score.hpp`
  and `stop_requested.hpp`, all reading the `StopState` of `stop_state.hpp`.
  `StopCondition` assembles them from `Limits` in the order it always checked, still samples
  the clock in batches, and takes the time fraction or the step fraction, whichever is
  further, along with the progress of any added criterion, as the progress of the solve. `Solver::add_stop_criterion` adds a `StopCriterion`
  (`stop/stop_criterion.hpp`) of the use case's own, asked after the built-in ones, made fresh
  for every solve and every start of a multi-start run. Every seeded solve gives the same
  result as before. `StopCondition` can no longer be copied (it may own added criteria).
- **Source layout: one file per construction heuristic; a use case's own construction.**
  `construct/first_fit.hpp` (assignment shape: first fit, first fit decreasing with a
  difficulty order) and `construct/cheapest_insertion.hpp` (sequence shape) hold the two
  heuristics, `construct/initialization_order.hpp` the order they share, and
  `construct/construction.hpp` keeps `Construction<Solution>::run`, which picks by shape, and
  `nothing_placed`. `Solver::set_construction` registers a construction in place of the
  built-in one, for every start of a multi-start run; it gets the solve's seed (each start's
  own) to draw with. The built-in construction is unchanged:
  every seeded solve gives the same result. **Breaking (C++):** `Construction<Solution>::
  place_best_value`, `place_cheapest_insertion_full_scan` and `initialization_order` are free
  functions (`optimaru::place_best_value`, ...).
- **The random draws are the same on every platform; every seeded result changes once.** The
  engine and the use cases drew through the `std::` distributions and `std::shuffle`, which
  libstdc++ (the Linux wheels, the API) and libc++ (the macOS wheels) implement differently, so
  the same seed and step limit could end differently on each, and the API's result could differ
  from a local solve on a Mac. Every draw now goes through `core/optimaru/utils/random.hpp`
  (`random_index`, `random_int`, `random_unit`, `random_real`, `random_bool`, `random_normal`,
  `random_shuffle`), written out by hand; `tests/test_layout.py` keeps the `std::` ones out and
  the engine test pins their first outputs. Floating-point arithmetic on top of the draws
  (`std::exp` in simulated annealing, fused multiply-adds) can still differ in the last bit
  between architectures. `random_index` of an empty range is 0, as `PortableRandom::next_int`'s
  was, instead of a division by zero.
  Five calls that drew twice in their arguments, whose order is the compiler's, draw in two
  statements: the meeting-scheduling room pick, and demo data (vehicle routing, pickup and
  delivery, employee scheduling, flight crew). The search is the same; its random numbers are
  not, so a seeded solve gives a different (equally good) result from 0.8.0's, and the vehicle
  routing and pickup-delivery demo instances are other instances. The engine tests that
  checked one seed for something about half the seeds reach (LNS from a local optimum, kicks
  from a warm start) now count over ten seeds, and the vehicle-routing ALNS check over ten.
- **Breaking (C++):** `movable_entity`, `apply_change` and `apply_swap` take no distribution
  argument.
- **Source layout: one file per move.** The ordinary moves of the move mix each have a file in
  `search/moves/`: `change.hpp`, `swap.hpp` (assignment shape), `relocate.hpp`,
  `element_swap.hpp`, `two_opt.hpp`, `or_opt.hpp`, `two_opt_star.hpp` and
  `cross_exchange.hpp` (sequence shape). `move_mix.hpp` keeps `MoveMixSampler`, which draws
  among them by weight, and `move_draw.hpp` holds what they draw with (`SampledMove`, the
  nearby pick). The granular descent of iterated local search, `GranularLocalSearch`, moves to
  `search/moves/granular_local_search.hpp`. The draws are the same in the same order, so
  every seeded solve gives the same result; `move_mix.hpp` and `iterated_local_search.hpp`
  still include what they used to provide. Nothing changes in the Python package.
- **Breaking (C++): `utils/` holds what the engine's folders share.** `portable_random.hpp`
  moves to `core/optimaru/utils/random.hpp` and `worker_threads.hpp` to
  `core/optimaru/utils/worker_threads.hpp`; the include guards follow
  (`OPTIMARU_UTILS_RANDOM_HPP`, `OPTIMARU_UTILS_WORKER_THREADS_HPP`). The top of
  `core/optimaru/` keeps the solver, its settings and its config. Nothing changes in the
  Python package or in what the solvers do.
- **Breaking (C++): no umbrella header.** `core/optimaru/optimaru.hpp` is gone. It included a
  dozen engine headers for convenience, but only employee scheduling's bindings and the engine
  test used it; every other use case includes the headers it needs, and those two now do too.
  C++ that included it includes `"optimaru/solver.hpp"`, `"optimaru/settings.hpp"` and the
  others it uses. The directory map it carried is the engine tree in `README.md`. Nothing
  changes in the Python package or in what the solvers do.
- **Breaking (C++): `accept/` and `select/` are folders of the engine again.** The acceptance
  rules and the adaptive operator selection are not operators the step loop draws: they decide
  whether it keeps a move and which operator it draws next, for moves and ruin-and-recreate
  alike. They move out of `search/` to `core/optimaru/accept/` and `core/optimaru/select/`;
  `search/` keeps the operators (`operators.hpp`, `strategies.hpp`, `moves/`,
  `ruin_recreate/`). C++ that includes them follows the paths (`"optimaru/accept/acceptors.hpp"`,
  `"optimaru/select/schemes.hpp"`), and the include guards are `OPTIMARU_ACCEPT_*` and
  `OPTIMARU_SELECT_*`. Bin packing's own acceptor moves the same way, to `bp/accept/`. Nothing
  changes in the Python package or in what the solvers do.
- **Breaking (C++): the step loop is the solver's.** `optimaru::LocalSearch` and
  `search/local_search.hpp` are gone. What the class ran was never one local search: it is the
  whole iteration of a solve (choose a move, score it, ask the acceptor, tell the selectors, and
  kick or start the next late-acceptance stage on a stall), so it is now `Solver::run`, in
  `solver.hpp`, and it holds no move. What the header carried beside the loop moved to the folder
  it belongs to: the ordinary moves to `search/moves/move_mix.hpp` (`MoveMixSampler`), the
  stagnation kick to `search/ruin_recreate/kick.hpp` (`StagnationKick`), the ruin operators of a
  solve and their adaptive selection to `search/ruin_recreate/ruin_pool.hpp` (`RuinPool`), the
  built-in strategies and the draw among them to `search/strategies.hpp` (`BuiltinStrategies`),
  the acceptor factory to `accept/acceptors.hpp` (`make_acceptor`) and the outcome of a
  candidate to `select/scheme.hpp` (`classify_outcome`). `Solver`'s public interface is
  unchanged, and so is every solve: the same seed and step limit give the same result. Nothing
  changes in the Python package.
- **Fix: a kick on a single entity or element.** With one entity (or one element) and kicks on,
  the number to ruin was clamped between 1 and `n - 1 = 0`, bounds `std::clamp` does not allow.
  What came out depended on the standard library: 0 on the Linux wheels (libstdc++), a kick that
  ruined nothing, and 1 on the macOS ones (libc++). Both now ruin one, so a seeded solve of a
  single entity or element with kicks on can end differently on Linux. The same went for C++
  callers that set `alns.max_ruin_count` below 1 (Python refuses that): libstdc++ handed the
  value itself to the ruin operators, a negative count; it is now 1.
- **Docs: how a solve runs, in the code.** `docs/how-it-solves.md` follows `Solver::run` step by
  step, in the order of the flowchart on optimaru.com, with the file, the settings and the way to
  extend each step; `Solver::run` marks the steps. Issue templates (a move or constraint idea, a
  bug) and a pull request template.

## 0.8.0

**From 0.7.0. Breaking:** the settings of a `SolverConfig` are grouped by the stage of the solve
they act in (`config.stop.time_limit_seconds`, `config.alns.min_ruin_fraction`,
`config.moves.change_weight`, ...), and so are a request's `solver_parameters` and the API's
`config` (`{"alns": {"min_ruin_fraction": 0.05}}`); a flat name is refused with its new place.
The C++ `optimaru::Settings` has the same groups. The search itself is unchanged: every use
case's seeded results are the same as 0.7.0's.

- **Breaking (C++): the engine's `Settings` has the same groups.** `optimaru::Settings` is laid
  out like the Python `SolverConfig`: `construction`, `stop` (was `limits`), `alns_trigger`
  (the kick's `enabled` and `unimproved_step_trigger`), `strategies` (the built-in strategies'
  weights and knobs, and `ruin_recreate_weight` / `ruin_recreate_max_ruin`, which were
  `ruin_recreate.as_move_*`), `custom_moves` (`share`, was `search.custom_move_probability`),
  `moves`, `selection` (`nearby`, `entity_hint`, `value_hint`), `acceptance`, `alns` (the rest of
  `search.ruin_recreate`), `run` (`seed`, `score_checks`, `parallel_starts`, `on_new_best`), and
  `unassigned_list`. `SearchSettings` and `RuinRecreateSettings` are gone. A use case's
  `Solver::Config` is unchanged. Nothing changes in a solve: every use case's seeded results are
  the same.
- **Breaking: `SolverConfig` settings are grouped by stage of the solve.** Every setting moved
  under one of `construction`, `stop`, `cost`, `alns_trigger`, `strategies`, `custom_moves`,
  `moves`, `selection`, `acceptance`, `alns` and `run`, in the order a solve meets them, and
  lost the prefix its group now says: `config.spent_limit_seconds` is
  `config.stop.time_limit_seconds`, `config.ruin_recreate_min_ruin_fraction` is
  `config.alns.min_ruin_fraction`, `config.change_move_weight` is `config.moves.change_weight`,
  `config.acceptor_type` is `config.acceptance.rule`. `optimaru._config.FIELDS` is the one table
  of the moves. A flat name raises `AttributeError` naming its new place, and a compiled
  setter's error names the grouped setting. `SolverConfig(stop={"time_limit_seconds": 10})` sets
  whole groups; `config.to_dict()` reads them. A request's `solver_parameters` and the API's
  `config` take settings by group (`{"alns": {"min_ruin_fraction": 0.05}}`); a flat key is
  refused with its new place. The JSON mirrors (`models.SolveConfig`, employee scheduling's
  `SolverSettings` and its `portfolio`) have the same groups; school timetabling's `weights`
  setting is `cost.weights`. Employee scheduling's `no_improvement_limit_seconds` and
  `no_improvement_step_count_limit`, old names of `stop.unimproved_time_limit_seconds` and
  `stop.unimproved_step_limit`, are gone. The solve keywords (`time_limit_seconds`,
  `random_seed`, `parallel_solver_count`, ...) and the C++ `Solver::Config` fields are unchanged.
- **Benchmarks: every table measured again, on one machine.** All 29 published data sets were run
  again on 2026-11-23/24 on the Apple M4 Max Mac the other tables came from, on optimaru 0.6.0,
  which searches as 0.7.0 does (the pickup-delivery CP-SAT rows on the model the benchmark now
  carries, with #144's horizon). The two VRPTW tables, school timetabling's track-1 table and the
  `ruin_recreate_as_move_weight` A/B runs in the meeting and conference scheduling guides came from
  a 4-core cloud machine before and now come from the Mac too, so no table or guide names another
  machine. `benchmarks/README.md` says how far the numbers repeated against the tables they
  replace.

## 0.7.0

**From 0.6.1. Breaking:** a request that sets a field which acts only through a cost, while its
constraints leave that cost out, is now refused with `ValueError` instead of solved as if the
field were not there; default selections are unaffected except employee scheduling's
`desired_shifts`. Routing profiles gain forbidden and preferred jobs (technician skills) and may
share travel (`same_travel_as`). The engine's C++ moved to `core/` and local search to `search/`;
nothing changes in the installed package's import paths.

- **Breaking: a parameter needs its cost.** A request that sets a field which acts only through a
  cost (a vehicle's `max_end_time`, a visit's `max_end_time`, a worker's `unavailable_dates`, a
  knapsack's `mandatory_items`, ...) while its constraints leave every such cost out is refused
  with `ValueError`, naming the field, where it is set and the costs to add; before, the solve
  ignored the field without a word. The effective selection counts (the defaults when
  `constraints` is None, a pick at weight 0 included). It is checked where the input is the JSON
  model (`solve_request`, `solve(model)`); compiled problems are solved as given. With an API key
  set, the request is sent as it is (school timetabling also checks before sending), and the
  refusal comes from the server once it runs this release. Default selections are unaffected
  (they hold every cost, pickup-delivery's opt-in `squared_order_age_cost` at weight 0, so
  `order_time` still needs that cost weighted to act), except employee scheduling's
  `desired_shifts`, whose `desired_shift_cost` is not among the defaults: a request that sets it
  now has to pick that cost. Each use case's
  `models.parameter_uses(model)` lists what an input sets, and docs/use-cases/README.md
  ("Parameters and their costs") has the table for all 22 use cases.
- **Routing: forbidden and preferred jobs per travel profile.** A vehicle routing
  `TravelProfile` lists the visits its vehicles may not serve (`forbidden_visits`) and those they
  are preferred for (`preferred_visits`); a pickup-delivery one does the same by request id
  (`forbidden_requests`, `preferred_requests`, both stops of a request kept together). Two new
  constraints in each use case score them: `profile_forbidden_visit_cost` /
  `profile_forbidden_request_cost` (hard, weight 1000, one unit per visit or per stop), which is
  always on: a constraint list that leaves it out, or picks it soft or at weight 0, still gets
  it hard at its defaults; and
  `profile_preferred_visit_cost` (soft, weight 600 per visit) / `profile_preferred_request_cost`
  (soft, weight 300 per stop), about ten minutes of driving for one job, which a request with
  preferred jobs on a profile some vehicle drives on must list (`ValueError` otherwise; a
  profile no vehicle drives on prefers nothing). A plan with such lists is scored on the
  default constraints, as one with groups is, on the constraint assembly, which takes fewer steps
  per second than the core path; the group operators and the pickup-delivery
  request operators never place a job on a vehicle that forbids it, and a job no vehicle may take
  stays unassigned. Unknown ids, an id twice in a list and an id both forbidden and preferred
  are refused, and so is a pickup-delivery request required on, or pinned to, a vehicle whose
  profile forbids it. The compiled plans take the lists as `ProfileVisits` / `ProfileRequests`
  entries (`profile_visits=` / `profile_requests=`), named per vehicle by
  `profile_visits_index` / `profile_requests_index`. On a plan without lists the search is the
  one it was. Engine-only: the CP-SAT reference has no plugin for them.
- **Routing: a profile may drive as another one does.** `TravelProfile.same_travel_as` names
  another profile whose `durations`, `distances` and `missing_value` this one uses, so profiles
  that differ only in their job lists (one per technician skill set, say) share one set of
  travel dicts. Profiles that drive alike share one matrix in the engine, and so do all the
  profiles that give no travel (straight lines), where each had a copy before: on a compiled plan
  such profiles now share one `Vehicle.profile_index`.
- **The engine's C++ moves to `core/`.** The header-only engine lives in `core/optimaru/` at the top
  of the repository instead of `optimaru/cpp/optimaru/`, so `optimaru/` holds the Python package and
  its use cases. Includes stay `#include "optimaru/..."`, and nothing changes in the installed
  package or its import paths; a build from source reads the headers from the new place.
- **Benchmarks: the pickup-delivery CP-SAT lane runs again.** Since 0.6 the lane failed: it solved
  with external-solver v0.3.0's pickup-delivery model, which reads the 0.5 input, and that library
  is frozen. The model is now kept in the benchmark (`benchmarks/pickup-delivery/cpsat`,
  `pickup_delivery_cpsat.model`), ported to depots, `start_depot` / `end_depot` and travel
  profiles, with a `travel_distance_cost` plugin; the expected-failure mark is gone, and its parity
  tests (external-solver v0.3.0's, ported, plus `tests/test_model.py` for depots and profiles) check
  it against the engine.
- **Source layout: the parts of local search live under `search/`.** In the engine,
  `core/optimaru/` keeps `search/local_search.hpp` and `search/operators.hpp` and now also
  holds what the step loop draws on: `search/moves/`, `search/ruin_recreate/`, `search/accept/`
  and `search/select/`, which were folders of their own next to `search/`. `construct/`, `stop/`,
  `score/` and `model/` stay where they were. A use case files its own moves and acceptors the
  same way: `<ns>/search/moves/` and `<ns>/search/accept/` (bin packing, knapsack,
  pickup-delivery, project job scheduling, school timetabling, semiconductor scheduling and
  vehicle routing have them). Nothing changes in the Python package or in what the solvers do; C++
  that includes these headers follows the new paths (`"optimaru/moves/list_move.hpp"` is now
  `"optimaru/search/moves/list_move.hpp"`, `"vrp/moves/group_moves.hpp"` is
  `"vrp/search/moves/group_moves.hpp"`).

## 0.6.1

**From 0.6.0.** School timetabling solves on the optimaru API like every other use case once an
API key is set, and its request takes a minimal starting solution. Nothing changes for a solve on
this machine; the routing notebooks follow the 0.6 input, and the docs site carries the optimaru
mark.

- **School timetabling solves on the optimaru API too.** With an API key set, `solve_request`
  sends its `TimetableRequest` to `POST /api/use-cases/school-timetabling/solve/jobs`, as the
  other request use cases do, and `solve(problem_dict, ...)` is sent as that request (the
  keywords, and `config` as the request's fields and `solver_parameters`) and returns the
  dict a local solve returns. Until now both solved on this machine with a `RuntimeWarning`.
  A `constraints=` selection travels as its picks whether it is specs, names or an
  `Assembly`; `on_new_best` is refused, as everywhere. `_api.local_solve` is gone: every use
  case is served.
- **School timetabling: `TimetableRequest.initial_solution` takes a minimal start.** Besides a
  whole `TimetableResultModel`, it takes `{"lessons": [...]}` whose lessons carry only a
  `course_id` and a `slot`, ignoring keys it does not know (`StartingSolutionModel`), as
  `solve(initial_solution=...)` always did. A server on this release accepts such a start in
  `request.initial_solution`.
- **Docs site: the optimaru mark.** The engine docs (GitHub Pages and optimaru.com/engine-docs/)
  show the optimaru mark as their favicon and header icon, on a plain white or black header with
  optimaru's green as the accent.
- **Routing notebooks on the 0.6 input.** `examples/vehicle-routing.ipynb` and
  `examples/pickup-delivery.ipynb` build their vehicles from depots (`start_location_index` /
  `end_location_index` in the compiled layer, `DepotModel` with `start_depot` / `end_depot` in
  the JSON models), explain travel profiles where they explained the driving matrix, print
  kilometers next to driving hours, and ask for optimaru 0.6.0 or later. Saved with outputs from
  0.6.0, they print and draw the same routes, scores and charts as on 0.5.1.

## 0.6.0

**From 0.5.2.** One breaking change: vehicle routing and pickup-delivery take depots, a start and
an end depot per vehicle, and travel profiles keyed by id in place of `return_to_depot` and one
driving matrix, so input written for 0.5 is refused (see the first entry). Nothing else changes
the package's input or results: `solve_request` no longer needs the package to be spelled
`optimaru`, the C++ sources moved into one tree per use case, and the benchmarks gain time-window
series for vehicle routing.

- **Vehicle routing and pickup-delivery: depots, start and end depots, travel profiles (breaking).**
  The input names its depots (`depots`, a list of `DepotModel` with an `id` and a `location`), and a
  vehicle names where its route starts (`start_depot`) and where it ends (`end_depot`, its start
  depot or another). Without an `end_depot` the route is open, as before without
  `return_to_depot`. `VehicleModel.depot` (vehicle routing), `VehicleModel.home` (pickup-delivery),
  `return_to_depot` and the input's `driving_seconds` matrix are gone; old input is refused. Driving
  comes from travel profiles: `profiles` maps a name to a `TravelProfile`
  whose `durations` (seconds) and `distances` (meters) are dicts keyed by depot and stop ids,
  `{from_id: {to_id: value}}`, and `VehicleModel.profile` picks one (`"default"` unless set). A pair
  the dicts leave out costs the profile's `missing_value`, 10,000,000 by default
  (`MISSING_TRAVEL_VALUE`); a profile without one of the dicts, and every vehicle when `profiles` is
  empty, drives straight lines between the coordinates (haversine meters, at 50 km/h for the
  seconds); an empty dict is refused, since it would make every pair missing. `to_plan` turns the
  dicts into one square matrix per profile over the depots, then the stops: for 1,000 places with
  both dicts given, 0.11 s to parse and validate 40 MB of JSON and 0.06 s to build the matrices on
  an M4 Max, so the JSON stays keyed by id. A new opt-in constraint,
  `travel_distance_cost` (soft, weight 0), prices the meters (its previews walk the candidate
  route, where `travel_time_cost` answers from prefix sums); the solutions report
  `total_driving_meters` and each vehicle's `driving_meters`, and `vehicle_metrics` no longer echoes
  `return_to_depot`. In the compiled layer `Vehicle` takes `start_location_index`,
  `end_location_index` (-1 for an open route) and `profile_index`, the plans take
  `durations=` / `distances=` (lists of matrices, one per profile; None for the straight-line
  fallback), and `driving_time` / the new `driving_distance` take an optional `profile`. With one
  profile the solvers search exactly as before (seeded, step-limited solves of both use cases,
  open and closed routes, both scoring paths, return the same routes and scores as before). The
  public CP-SAT library the pickup-delivery benchmark pins (external-solver v0.3.0) is frozen and
  reads the 0.5 input, so that benchmark's CP-SAT lane is marked as an expected failure. The two
  routing notebooks ask for `optimaru<0.6` until they are ported to the new input.
- **`solve_request` wrappers: the package is no longer written out.** The decorator behind the
  `solve_request` of the request use cases refused a function that was not under a package
  spelled `optimaru`. It now reads the package from its own module. Nothing changes for an
  installed optimaru; a copy of the package under another import name no longer fails when
  one of those use cases is imported.
- **Source layout: one C++ tree per use case.** `optimaru/<use case>/cpp/include/<ns>/` and
  `cpp/src/` are merged into `optimaru/<use case>/cpp/<ns>/`, where a header sits next to its
  source, filed by role under the engine's folder names: `solver` and `constraints.hpp` at the
  top, `model/` (`domain.hpp`, demo data, travel times), `score/` (the monolithic calculators),
  `constraints/`, and `moves/`, `construct/`, `accept/` where a use case has its own. The demo is
  `cpp/main.cpp`. Nothing changes in the Python package or in what the solvers do; C++ that
  includes a use case's headers follows the new paths (`"vrp/domain.hpp"` is now
  `"vrp/model/domain.hpp"`, `"vrp/score_calculator.hpp"` is `"vrp/score/score_calculator.hpp"`),
  with `optimaru/<use case>/cpp` as the include directory.
- **Benchmarks: vehicle routing with time windows.** `benchmarks/vehicle-routing` runs two new
  series, `--series solomon` (Solomon's 56 instances of 100 customers) and `--series homberger`
  (Gehring & Homberger's 300, 200 to 1000 customers), against the OR-Tools Routing solver with a
  time dimension. They are scored by the DIMACS convention under which CVRPLIB lists its
  best-known solutions: distance alone, every distance truncated to one decimal (the harness
  works in tenths). The files and best-known solutions are read at a pinned commit of the
  PyVRP/Instances repository, and the rescoring replays each route's schedule, so a missed
  window is reported as infeasible. `--jobs N` runs N single-threaded runs side by side.
  `scripts/publish_benchmarks.py` reads `vrptw-solomon.json` and `vrptw-homberger.json` into a
  results block of their own and two homepage data sets, and every homepage data set now names
  the machine it was measured on (`--machine`, so one publish run takes one machine's results). A
  run that is both incomplete and over capacity now prints `n unassigned` rather than `infeasible`,
  as the publisher already did. The publisher refuses a sampled run (`--limit-per-bucket`) of
  Set X or Gehring & Homberger, whose tables would read as the whole series.

## 0.5.2

**From 0.5.1.** Two additions: `solve` and `solve_request` can run on the optimaru API with an API
key, and project job scheduling gets a move that rebuilds the schedule from its order (on by
default, so its results differ from 0.5.1). The rest is benchmarks, CI, tests and notebooks.

- **Solving on the optimaru API with one setting.** The optimaru API is the Enterprise
  edition: this engine run as a hosted service, whose solve request is the Python call. With
  an API key, in the environment (`OPTIMARU_API_KEY`) or in code
  (`optimaru.use_api("om_live_...")`), `solve(model, ...)` and `solve_request(request, ...)`
  send the problem there, wait for the solve job and return what they return on this
  machine: a new solved model, or the response model. `OPTIMARU_API_URL` or
  `use_api(key, url=...)` names another API than `https://app.optimaru.com`, and
  `optimaru.use_local()` solves here again. `config` travels as the `SolverConfig` fields
  that differ from the defaults and `constraints` as the picks in their order, so the same
  seed and step limit give the same result there as here on the same engine version. What
  cannot be sent is refused, not solved here without notice: a problem built from the
  compiled classes (pass its model), the `solve(problem)` of vehicle routing, pickup and
  delivery, order picking and employee scheduling (call `solve_request`), a callback
  (`on_new_best`, `select`), a non-finite number and a setting the engine refuses. School
  timetabling keeps solving here, with a `RuntimeWarning` once (the API takes it in a request
  of its own); the compiled `Solver` class used directly is the engine itself and always
  runs here. What the API refuses or fails at raises `optimaru.ApiError` (`status`, `code`,
  `detail`, `retry_after`). A solve is a job the call waits for, for at most twice its time
  limit (times its parallel starts, for a request) and ten minutes more: a look at it that
  fails for a reason that passes (the network, a 5xx, a 408, a 429) is tried again for a
  tenth of that wait and at least a minute, as is the read of its result,
  and interrupting a solve once its job exists cancels it. The key goes to that API only
  (https, or http to localhost without a proxy; no redirect followed), and the first solve
  sent there is logged at INFO on the `optimaru` logger. No new dependency: the standard
  library and pydantic. Without a key nothing changes and nothing is sent. Every use case's
  `solve`, and the `solve_request` of the four request use cases and of school timetabling,
  are wrapped for this (`__wrapped__` is the function that solves here); their signatures
  are unchanged.

- **CI: benchmarks run when a pull request touches them.** A CP-SAT comparison job (and the
  vehicle routing benchmark job) runs on a pull request that changes its benchmark project or
  its use case, all of them when the shared runner (`benchmarks/cpsat-common/`), `ci.yml` or
  the Python and build files every benchmark installs (`optimaru/*.py`, `setup.py`,
  `pyproject.toml`, `CMakeLists.txt`) change, and all on a manual run of CI and on a weekly
  one (Sunday 18:00 UTC). A change to the C++ engine alone runs the three jobs whose C++
  bench checks scores against Python (conference and meeting scheduling, school timetabling)
  and no other. A push to `main` runs the CP-SAT jobs only up to their install, which keeps the
  build caches warm for pull requests. A smoke run takes one instance per scale
  (`--one-per-scale` skips the seeds `seeded()` repeats a scale on), and the school
  timetabling job also runs track 1's cross-check gate. `school-timetabling-bench.yml`
  and `vrp-public-bench.yml` are run by hand only. A CI run took 7 to 9 minutes on every pull
  request, most of it in the smoke runs; one that changes no benchmark now waits only for the
  build and test jobs (the longest about 2.5 minutes).

- **Project job scheduling: a move that rebuilds the schedule from its order.**
  `SerialScheduleMove` (`SolverConfig.serial_schedule_moves`, on by default; drawn on at most
  a fifth of the steps, `custom_move_probability = 0.2`) reads the start dates of the working
  schedule, changes the order they give (one allocation's place, a few neighbours, a
  precedence chain or a project, an execution mode, or the order of the schedule pushed to
  the right) and places every allocation again as early as its predecessors and the
  renewable resources allow, late projects first. Single changes of a delay stalled some
  percent above what the order allows. After 30 s on one thread the RG30 makespans are 63 /
  75 / 55 / 73 / 72 (66 / 82 / 59 / 76 / 75 before; OR-Tools CP-SAT on 8 workers 62 / 74 /
  56 / 73 / 71), the 20 generated instances of that shape end shorter on all 20 (21% in
  sum), and the comparison ladder is better on 9 instances and the same on 11. A seed's
  result differs from 0.5.1; `serial_schedule_moves = False` gives the earlier search.

- **Benchmarks: every table has 20 instances or more.** The 17 generated comparisons run
  ladders of 20 instances: a few sizes with several seeds of the generator each, the first
  instance of a size keeping its name and the others numbered from 2 (`medium-2`, ...;
  `cpsat_plugins.benchmark.seeded`), and food packaging 20 plants of different sizes. The
  RCPSP runner generates instances in the shape of RG30 (`--synthetic N`,
  `--synthetic-seeds`), published as a second table beside Pat1-Pat5 (20 instances of 30 to
  120 activities); the synthetic pickup-delivery table has one row per size and instance
  seed (21 rows, 10 to 400 requests; `--synthetic-seeds`); school timetabling's track 2 runs
  20 schools of 3 to 60 classes (`--sizes ladder`, or any class count that is a multiple of
  3) and writes its results as JSON (`--out`), which `scripts/publish_benchmarks.py` reads as
  `timetabling.json` in place of numbers copied by hand (an `extra.json` is refused). Track 2's
  CP-SAT time (`wall`) now includes building the model, so it is not comparable with older
  `RESULTS_track2.md` files. The Li & Lim head-to-head table adds
  the first 15 requests of each instance.
- **Benchmarks: every instance is generated in Python.** The C++ quality benches read
  instance files and generate nothing; no solver, constraint or published comparison table
  changes.
  - School timetabling, track 1: `school_timetabling_cpsat/gen_quickstart.py` generates the
    S/M/L schools and the fixed cross-check instance, and writes each as JSON (CP-SAT, the
    evaluator) and as a token file (`.sttxt`, the C++ bench). `st_quality_bench` is now
    `st_quality_bench <instance.sttxt> <seed> <seconds> [parallel_solver_count]` and
    `st_quality_bench crosscheck <crosscheck.sttxt>`; `school-timetabling-compare` writes the
    instances itself, as before from `--seed`. The bench used to build them with
    `std::shuffle`, which gives another school per standard library; the Python generator
    draws from `random.Random(seed)` alone. The presets are the same, the schools are new
    ones: `RESULTS.md` has a track-1 table on them (the engine's S 175 / M 350 / L 666 soft
    at 30 s) and says which instances each older table is of. Track 2 and track 3 already
    generated in Python and are unchanged, as is the homepage's table (track 2).
  - Conference scheduling and meeting scheduling: `cs_quality_bench` and `ms_quality_bench`
    take a dumped instance of the CP-SAT comparison (`<instance.cstxt|.mstxt> <seed>
    <seconds> [knobs]`) in the place of `demo` and `scaled:N`, whose generators are gone
    (0.5.1 is the last release with them; they are also in the `v0.5.0` tag). `benchmarks/<slug>/dump_instances.py` writes the
    instances, as bed allocation's does, so a tuning run and the comparison solve the same
    problems. `<ns>_quality_bench crosscheck <file>` scores a fixed assignment, and the dump
    script prints the use case's own score of it: `--check <bench>` compares the two, and
    every constraint's raw cost, on every instance, and CI's `benchmark-cpsat` job runs it.
  - The three readers refuse an index outside its table, and a seed or a budget that is not
    a whole number.
  - The guides keep the tuning history measured on `scaled:N` and say where those instances
    came from. Each has a new table of the as-move default against `asMove=0` on the
    comparison's instances: it wins on the smaller ones and not on the largest two of either
    use case (meeting scheduling's `xlarge`: a mean objective 17.5% worse over 12 seeds).
    No default changes with this.
  - `tests/test_layout.py` holds a C++ bench to reading a file that a Python writer names.
- **Tests and docs pick the defaults by name, not by position, in every use case.** Lists
  such as `[*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)]` or `[:3]`, `[:9]`,
  `[1:]` meant "every default, with this one replaced"; a constraint added to the catalog,
  as the extending guides teach, would have been silently dropped or would have displaced
  another pick (#117 fixed maintenance scheduling). The tests of 16 use cases, the snippets
  in the READMEs and `python.md` of seven, the docstrings of five packages and a CP-SAT
  parity test now select by name in catalog order: the docs with the plain comprehension
  (`[X(...) if spec.name == "x_cost" else spec for spec in get_constraints()]`), the tests
  with `tests/_picks.py` (`replace_default`, `without`, `only`), which fails on a name that
  matches no pick or several. Meeting scheduling's and tournament scheduling's raw-cost
  metadata tests took their unit constraints as the first five or two of
  `list_constraints()`; they now name them (`UNIT_CONSTRAINTS`), and their guides say to
  add a new unit constraint's name there instead of editing a slice. The five
  extending guides that explained a slice (and employee scheduling's, which named one in a
  test) no longer do. No expected score changed: the picks are the same lists.
  `tests/test_layout.py` fails on an index or slice of `get_constraints()`, `constraint_info()` or
  `list_constraints()` in the tracked tests, docs, package sources, benchmarks, notebooks
  and the root README.

- **Notebooks on 0.5.1.** Every notebook in `examples/` is saved with outputs from optimaru
  0.5.1 and asks for it; `examples/README.md` names that version. Facility location's next
  steps name `swap_facility_moves`, and bin packing's description of its search names the
  trade move. Twelve of the twenty-two print and draw exactly what they did on 0.5.0. Bin
  packing and facility location print new numbers from their new moves: bin packing's rush
  drums are now repaired by the search, as its text now says. With warm starts fixed (#110)
  the restart experiments moved for 0.5.0 were run again. Food packaging's rounds go back to
  50,000 steps, where they show their point and print what they printed on 0.4.3; sports
  league scheduling's new-seed restarts climb again, and its text no longer says that a
  restart stalls at once; it says what a climbing new-seed line means. The others keep the
  step budgets that make their cells repeat (knapsack's rounds and move sets, food
  packaging's rush-order replan, maintenance scheduling's runs, rounds and urgent-job
  replan); bed allocation's new admissions keep seed 3, since seed 7 returns one plan at
  every budget, and drop the 3,000,000-step restart, which returned the 100,000-step plan;
  its text now says a long restart can drift as far from the old plan as a solve from
  scratch.

## 0.5.1

**From 0.5.0.** One fix first: in the seven use cases that search in stages, a solve that
starts from a given solution (`with_initial_solution`, a request's `initial_solution`)
searches as in 0.4.3 again; 0.5.0 could return the given solution unchanged (#110). What
else changes a result: bin packing and facility location have new moves, so a seed's
result there differs from 0.5.0; tournament scheduling searches a solve with
`constraints=` as it searches the default one, so a seed's result with `constraints=`
differs; and in employee scheduling the default solve no longer judges a pinned shift on
the four per-shift constraints, as the docs and the solve with `constraints=` already had
it, so a schedule whose pinned shift breaks one of them scores 0 there where it scored
`-1hard`. What changes without changing a result: a solve with constraints is faster where
some of them are at weight 0, with the same scores for a seed and a step limit; school
timetabling solves with a new soft constraint that has no weight field, where it raised;
and every demo instance is `generate_demo_data()`, with the five earlier names kept as
aliases. The entries on notebooks, benchmarks, tests and guides are about the repository,
not the wheel.

- **Benchmarks: external-solver `v0.3.0`.** The benchmark projects pin the external-solver
  release that follows optimaru 0.5, the links into its sources point at that tag, and their
  lock files record the version of this checkout (0.5.1 with this release). In `v0.3.0` the
  worker count of a CP-SAT `solve` is `parallel_solver_count`, the name of the engine's
  keyword (`num_workers` is still read), and its employee scheduling plugins say constraint
  where they said rule (`SchedulePlugin` was `RulePlugin`); the runners and the READMEs here
  use the new names. No table changes: the CP-SAT models are unchanged since `v0.2.3`.
- **Notebooks on 0.5.0.** Every notebook in `examples/` is saved with outputs from optimaru
  0.5.0 and asks for it; `examples/README.md` names that version. The four that build a request
  use the names of 0.5: `time_limit_seconds`, and in employee scheduling
  `default_constraints()`, `model_input.constraints` and `constraint_totals`. Twelve of the
  twenty-two print and draw exactly what they did on 0.4.3, and four move within the noise of
  solves that stop on the clock. The other six are of use cases whose search changed, and some
  of their experiments no longer showed their point; those now run on a step budget or another
  seed: knapsack's comparison of move sets (20,000 steps each, `core_repack_moves` off beside
  the other moves) and its restart rounds, bed allocation's new admissions, food packaging's
  rounds and rush-order replan, maintenance scheduling's short runs, restart rounds and
  urgent-job replan. Maintenance scheduling's comparison of ruin and recreate on and off is
  gone: over 8 and 12 seeds it makes no clear difference there on 0.5.0.
- **Fixed: a solve from a given solution in the seven use cases with stages (#110).** In
  0.5.0 bed allocation, food packaging, knapsack, maintenance scheduling, max-cut, quadratic
  assignment and sports league scheduling ran late acceptance in stages on every solve, also
  one that starts from a solution the caller gave (`with_initial_solution`, a request's
  `initial_solution`). A stage starts over from the start's score, which on such a solve is
  where the search already stands: it loosened nothing and was taken at each stall in the
  place of the ruin-and-recreate kick. A solve whose improvements come from the kicks then
  returned its start once the budget had room for stages.
  - Now only a solve that the construction starts has stages (`optimaru::nothing_placed`, or
    the use case's own seed flag). A solution that places only a part counts as given.
  - The engine also ends the stages by itself when a search stalls before it has found
    anything better than its start, whatever started it: a construction that no move improves
    has none either, which matters to a caller who sets `AcceptanceSettings::history_growth`
    directly.
  - Measured, a food packaging week with one new job, four seeds: improved on 4 of 4 at every
    step limit in 0.4.3 and again now, on 0 of 4 from 200,000 steps up in 0.5.0.
  - Measured, a bed allocation plan with five new stays, 12 seeds: a mean medium score of
    -10.33 after 10 million steps in 0.4.3 and now (the same table, seed by seed), -10.58 in
    0.5.0, where 2 of the 12 seeds ended worse under a larger step limit than under a smaller
    one.
  - On 36 instances of the seven benchmark ladders a solve from a given plan finds exactly
    what a build without stages finds, new best by new best. The 46 instances solved from
    the construction end on the same score as in 0.5.0 at 300,000 and at 3 million steps, so
    the benchmark tables stand.
  - The notebooks' restart experiments were moved to step budgets for 0.5.0 (the entry
    above) and have not been run again on this.
- **Bin packing: `TradeMove` and a fill tie-break.** `used_bins_cost` counts bins, so every
  packing with the same number of bins has the same score, and the search wandered from one
  to the next: on Falkenauer's triplets it ended one bin above the best known count. Two
  additions, both on by default. `TradeMove` (`bp/moves.hpp`, `SolverConfig.trade_moves`)
  trades up to two items of one bin for up to two of another so that the fuller of the two
  bins gets fuller, or, when no trade does that, so that both keep their loads with other
  items. And late acceptance takes a step that keeps the score when it does not lower the sum
  of the squared bin loads, and otherwise one time in eight (`bp/fill_late_acceptance.hpp`,
  `SolverConfig.fill_tie_break`). That sum is not part of the score: the score, the limits
  and `on_new_best` are as they were. On the 24 sampled Falkenauer instances (two seeds,
  10 s) no run ends above the capacity bound (39 of the 48 did before). The bound is the
  best known count the OR-Library file states for 23 of them, and one below it for
  `u120_19`. A seed's result in bin packing differs from 0.5.0.
- **Benchmarks: the bin packing table measured again.** With the two additions above the
  engine uses the best known number of bins the OR-Library file states on 23 of the 24
  Falkenauer instances of the table (on 6 before) and one bin fewer on `u120_19`, and CP-SAT
  is ahead on none (on the six triplet instances of 60 and 120 items before). The table of
  generated instances in `benchmarks/bin-packing` is measured again as well, with each part
  of the search switched off in turn.
- **Facility location: `SwapFacilityMove`, and a solve from scratch in stages.** The search
  could close a facility (`CloseFacilityMove`) but not put another in its place, so it kept
  the facilities it first settled on: on the 200-consumer comparison instance it ended with
  11 of CP-SAT's 12 facilities, 1.4% behind. `SwapFacilityMove`
  (`SolverConfig.swap_facility_moves`, on by default) opens a closed facility and closes the
  one that serves its surroundings in one move; the closed one's consumers go to their
  nearest facility with room, and each facility with room takes the consumers nearer to it.
  A solve that starts from nothing now also restarts late acceptance with a doubled history
  when it stalls (`late_acceptance_size` is the first stage's length), and from the first
  restart on it draws the macro moves on a tenth of the steps `custom_move_probability`
  allows. A solve from a given assignment has no stages and keeps the full gate.
  - Measured on the six instances of `benchmarks/facility-location` (30 s, three seeds): the
    mean cost above CP-SAT's is 0.07%, from 1.91%.
  - A short solve: after 50,000 steps the mean cost of a 200-consumer instance over 8 seeds
    is 3.10 million, against 3.22 million; it is lower at every step limit from 20,000 to 2
    million, also with 30 and with 1,200 consumers.
  - From a given assignment (36 seeds, 300,000 steps): 0.15% to 0.80% above CP-SAT's cost on
    the 60- and the 200-consumer instance, against 2.40% to 3.00%. A plan that needs one more
    facility for its new consumers keeps its facilities, as before: no move opens a facility
    without closing another.
  - A seed's result in facility location differs from 0.5.0.
- **Engine: `AcceptanceSettings::later_stage_custom_move_share`.** With `history_growth`,
  the gate on a use case's own move operators (`custom_move_probability`) is multiplied by
  this once late acceptance has started over for the first time. A later stage takes worse
  solutions for a long while, the adaptive selector counts outcomes and not what a move
  costs, and macro moves that each make a pass over the solution then take the steps of the
  moves that polish; the first stage, the descent from the start, keeps the gate as set. 1
  by default (no change); facility location sets 0.1.
- **Benchmarks: the facility location table measured again.** The engine has CP-SAT's score
  on the four instances up to 200 consumers (on two before) and is 0.02% and 0.29% behind on
  the two larger ones (1.4% to 3.3% behind on four before).
- **Python: every demo instance is `generate_demo_data()`.** The function that returns a use
  case's demo instance had five names; six use cases already called it `generate_demo_data`,
  five did not: `order_picking.build_demo_data`, `meeting_scheduling.demo_data` (the package,
  `models` and `_core`), `tournament_scheduling.models.demo_data`,
  `conference_scheduling.demo_conference` and `sports_league_scheduling.demo_schedule`. They are
  `generate_demo_data` now, with the same arguments and the same instance. The five old names
  remain as plain aliases of it in the modules that had them, without a warning, and are no
  longer in `__all__`. `tests/test_layout.py` checks from the source that a demo function has
  no other name.
- **Tests: maintenance scheduling picks the defaults by name, not by position.**
  `test_offset_zero_reproduces_the_plain_scores` took `get_constraints()[:5]` as "the four
  date constraints and `crew_conflict_cost`", so a constraint added to the catalog would
  have silently replaced one of them. It now takes every default with the tag conflicts
  squared and states offset 0 on each, so a new constraint is covered without a change; the
  snippet in `python.md` and the step in `extending.md` that warned about the slice follow.
- **Tests: the pickup-delivery and order-picking plugin tests compare a solve with an
  independent recompute.** Check (f) of `pdp_plugin_test` and `op_plugin_test` compared the
  solution's score with `ScoreCalculator::score()` of the same solution, but that call
  stores its result in the solution first, so both sides were one number. The check now keeps
  the score the plugin solve reported and compares it with the monolithic recompute; with
  the travel or walking weight doubled in the assembly it fails, as it passed before.
- **Employee scheduling: the default solve no longer judges a pinned shift on the per-shift
  constraints.** `es.solve(schedule)` and `es.calculate_score()` counted a pinned shift in
  `required_skill_cost`, `employee_unavailability_cost`, `undesired_day_cost` and
  `desired_day_cost`, while the components behind `constraints=` and `solve_request`, the
  constraint descriptions and `python.md` treated it as settled history that is not judged
  again. So the same schedule scored differently on the two paths: a pinned shift on a skill
  its employee lacks scored `-1hard` on the default solve and 0 with `constraints=`. The
  default solve now skips it too, so that schedule scores 0 on both, and a pinned shift on an
  unavailable, undesired or desired day no longer costs or earns its minutes there. The
  constraints between shifts (`shift_overlap_cost`, `max_one_shift_per_day_cost`,
  `min_rest_cost`) and `shift_count_imbalance_cost` count a pinned shift on both paths, as
  before. A schedule without pinned shifts scores as before; no benchmark instance pins a
  shift. `es_plugin_test` now pins shifts in its parity check and solves with pinned
  violations under `ScoreChecks::Full` on both paths.
- **Benchmarks: every public-instance runner has one form.** Pickup & delivery and
  project-job scheduling ran theirs as a command of the comparison package, while bin
  packing, quadratic assignment and assembly-line balancing kept a loose `bench/` package
  beside the project, run as `uv run --project cpsat python -m bench.run_bench`. Those three
  now follow the first two: `<use case>_cpsat/public_benchmark.py` (helpers in
  `public_benchmark_data.py`) with the command `<slug>-public-bench`, run with `uv run` from
  `benchmarks/<slug>/cpsat`; `bench/RESULTS.md` is `cpsat/PUBLIC_BENCHMARKS.md`, downloads
  are cached in the package's ignored `.cache/`, and bin packing's search-quality check is
  `bin-packing-quality-bench` (`bin_packing_cpsat/quality_bench.py`). Flags, output and
  `--json` are unchanged; the tests are `cpsat/tests/test_public_benchmark.py` and need no
  network. `tests/test_layout.py` checks that no `benchmarks/<slug>/bench/` package is left
  and that every `public_benchmark.py` has its `<slug>-public-bench` entry. A checkout that
  ran the old runners keeps its downloads in `benchmarks/<slug>/bench/.cache/`: still
  ignored, no longer read, so delete that `bench/` directory or move the archives to
  `cpsat/<use case>_cpsat/.cache/`.
- **Benchmarks: school timetabling's comparison is a package with a command.** Its
  three-track harness was a separate script project (`benchmarks/school-timetabling`,
  `st-bench`, not installed) whose four scripts imported each other as top-level modules and
  put the CP-SAT model and the engine on `sys.path` by hand, run as `uv run run_bench.py`.
  It is now the package `school_timetabling_cpsat` in the `benchmarks/school-timetabling/cpsat`
  project, next to the CP-SAT model `teacher_scheduling`, as every other use case has it:
  `run_bench.py` is `benchmark.py`, run as `uv run school-timetabling-compare` (`--track2`,
  `--track3`), and `evaluate`, `cpsat_quickstart` and `gen_canonical` run as
  `uv run python -m school_timetabling_cpsat.<module>`. The `st-bench` project and its lock
  are gone, one `uv sync` there builds the engine extension too, and
  `school-timetabling-bench.yml` syncs and runs only that project. Flags, outputs and the
  generated instances are unchanged (the S, M and L schools are byte-identical); `out/` is
  now under `cpsat/` when run with the default `--out-dir`.
- **Tournament scheduling: `TeamCountMove` and the guided change values run with picked
  constraints too.** `ts.solve(schedule, constraints=[...])` searched without the macro move
  that empties or refills whole teams, because the move read the monolithic calculator's
  counters, so on the demo it stayed on the 7-team optimum (medium 1.19523) where the default
  solve reaches medium 0 (6 teams of 12 slots). The move now reads its counts through a small
  type: the calculator's on the default path, as before, and counts of its own on the
  assembly, recounted from the schedule each time the move is drawn; `value_recommender`
  counts the day's teams off the schedule there. `team_count_moves` and `value_recommender`
  now apply to both paths, and the same seed and step limit end on the same schedule on both.
  - Demo, 1 million seeded steps: with the default constraints picked, medium 0 (soft
    -1.54919), from medium 1.19523; with `team_count_moves` off, medium 1.19523 on both paths.
  - The default path is unchanged: the same final score and the same number of new bests
    from the same seed and steps (demo and the 800-slot `xlarge` instance), and 2.33 million
    steps per second on `xlarge` before and after (medians of three seeded runs at 4 million
    steps, interleaved).
  - The assembly pays for the move as the default path does: 1.20 million steps per second
    on `xlarge`, from 1.73, and a better end after 1 million seeded steps (soft -6.97615,
    from -9.31891, which the default path also reaches).
  - A seed's result with `constraints=` differs from 0.5.0.
- **School timetabling: a new soft constraint no longer needs a fast-path weight field to
  solve.** Python read every soft constraint's weight off `canon.ObjectiveWeights` by name,
  on both paths, so a soft component added to the catalog made every `st.solve()` raise
  `AttributeError` until it had a field there (C++ struct, binding, stub), which the guide
  made part of its first step. Now a solve with `constraints=` weighs it at the pick's
  weight, and the default path, whose monolithic calculators do not score it yet, reports it
  in `penalties` and weighs it at its component default in `objective_value`, as the field
  will; `weights=` and `models.ObjectiveWeightsModel` refuse any other value for it (after
  the usual checks: a bool, NaN or a negative weight raises as anywhere else). A constraint
  with a field is read as before, and a field the catalog does not know fails the import. The example of
  `docs/use-cases/school-timetabling/extending.md` adds the field in step 5, with the
  monolithic terms; its steps 2 to 5 were applied, built and tested again.
- **School timetabling docs: the speed of a solve with `constraints=` as measured.** The
  python guide said the assembly runs at 35–55% of the monolith's move throughput (track 2
  at 10 s), and a fixed-step run on 0.4.3 had shown about 25%. Seeded single-thread solves
  of the track-2 schools (S and M 2 million steps, L 4 million, seeds 1 to 3) give a median
  of 46% on S, 62% on M and 37% on L, and 25% to 71% for one seed: the rate of both paths
  moves with the search. The python guide, the extending guide's step 5 and the track-2
  notes in `benchmarks/school-timetabling/RESULTS.md` now say so.
- **Engine: a constraint at weight 0 costs a move nothing.** An opt-in constraint that is off
  (weight 0) adds an exact 0 to the score, but a solve with constraints still computed it: on
  an assignment-shaped use case every move called its `delta_cost()` or `cost()`, on a
  sequence-shaped one every applied list edit did (the previews already left it out), and a
  global was sent every change. So `get_constraints()`, which holds the opt-in constraints at
  their weight 0, ran slower than the same picks without them, and slower with every opt-in
  constraint added.
  - A unit constraint at weight 0 is now left out of every move: member changes, list edits,
    batch ends, previews and the trial snapshot. Only a reset costs it, and
    `constraint_costs()` costs it on the current solution when it reports, so its raw cost is
    still reported. A report right after a reset costs nothing extra, which is what `solve`
    and the Python `constraint_costs()` do; a C++ caller that reports between moves pays one
    `cost()` per unit for each such constraint, every time.
  - A global is still sent every change by default, because a global may serve more than its
    own cost (aggregates a move source or another component reads). One that only feeds its
    own `cost()` can declare `idle_at_weight_zero()` (`GlobalConstraint`, described in
    `docs/extending.md`): at weight 0 it is then sent no change and is reset before its raw
    cost is reported. Assembly-line balancing's `workload_imbalance_cost`
    and max-cut's `partition_imbalance_cost`, the two globals with a default weight of 0, do.
  - A member of a compiled delta group is still evaluated with its group at weight 0; only its
    fold is left out.
  - No score changes, bit for bit: the weighted constraints are visited in the order they
    always were. Checked on seeded, step-limited solves with `get_constraints()`, three seeds
    each (assembly-line balancing 1 million steps, max-cut 2 million, vehicle routing and
    pickup and delivery 300,000): the final score and the sequence of new bests are those of
    main before this change. A solve without `constraints=` does not go through this code.
  - Measured on the same solves, one thread, medians of six runs with before and after taken
    in turns: assembly-line balancing (100 tasks) from 1.78 to 4.51 million steps per second,
    pickup and delivery (100 requests, 10 vehicles) from 54 to 70 thousand, max-cut (400
    vertices) from 3.53 to 3.59 million. Vehicle routing (100 visits, 10 vehicles) stays where
    it was, 14 and 13 thousand, inside the spread of its runs (12 to 14).
  - The step-by-step guides said that a pick at weight 0 is still computed and measured what
    it cost; those passages and their figures are re-measured on this version.

## 0.5.0

**Upgrading from 0.4.** Two things can break a caller; the entries below have the details.
pydantic 2.11 or newer is required (was 2.5). And JSON written by the models uses the new
names only: `time_limit_seconds` and `parallel_solver_count` in a request (were `timelimit`
and `num_workers`), `constraints` and `constraint_totals` in employee scheduling (were
`rules` and `rule_totals`). The earlier names are still read, in JSON, as keywords and as
attributes, so what a caller sends keeps working, and employee scheduling's Python names
with "rule" in them (`default_rules()`, `Rule`, `RULE_MODELS` and the others) are the
constraint names under a second name. Seven use cases search differently, so a seed's
result there differs from 0.4.3: bed allocation, food packaging, knapsack, maintenance
scheduling, max-cut, quadratic assignment and sports league scheduling. So does a solve
with constraints in vehicle routing or pickup and delivery that asks for more than one
parallel start: it runs them now. The entries on notebooks, benchmarks and guides are
about the repository, not the wheel.

- **Vehicle routing and pickup and delivery: parallel starts on a solve with constraints.**
  `parallel_solver_count` (also a request's field; `num_workers` is its earlier name) had no effect when a plan was scored
  with constraint plugins: a list passed as `constraints`, a request with a vehicle limit, a
  pinned stop or a required vehicle, or a plan with groups. Those solves now run that many
  starts, start *i* on `random_seed + i` and on its own copy of the constraints, and return
  the best, as a solve on the core rules does. With one start nothing changes. On generated
  grouped plans four starts end 0.8% to 2.4% shorter than one in the same 4 s (vehicle
  routing, four classes) and 2.1% shorter (pickup and delivery), longer in none of the 25
  pairs. In C++,
  `Solver::solve(plan, makeCalculator)` takes a factory that builds one calculator per
  start; the overload that takes a calculator stays a single start. Vehicle routing's
  `solve_request` leaves visit groups to the engine, as pickup and delivery's does since
  0.4.3. Both solvers refuse a bad `SolverConfig` before the plan is touched: the check ran
  after the unassigned pool was cleared, and in pickup and delivery after the pair
  construction.
- **Notebooks.** Every notebook in `examples/` is saved with outputs from optimaru 0.4.3 and
  asks for it; `examples/README.md` names one version again. Nineteen of the twenty-two show
  what they showed before. Three move within the run-to-run noise of solves that stop on the
  clock, and no text quotes a number that moved: conference scheduling and meeting scheduling
  print slightly different scores, and order picking's chart of rounds takes another path
  through its middle rounds to the same printed end.
- **Benchmarks: the lock files record optimaru 0.4.3.** The benchmark projects install
  optimaru from this checkout, and their locks name its version: they still said 0.4.2 after
  the release, so the first `uv sync` rewrote them. Nothing else moves; the projects stay on
  external-solver `v0.2.3`, which needs no change for 0.4.3.
- **`solve` for a problem, `solve_request` for a pydantic request, in every use case.** The
  pydantic path had two shapes: four use cases took a request through `models.solve_request`,
  the others took their problem model through `solve`, and school timetabling took dicts.
  Every use case now has both entry points, and the package exports both
  (`optimaru.<use case>.solve_request` is `models.solve_request`).
  - *New:* a request model in the other 17 and in school timetabling: `models.<Name>Request`, the problem model as
    `model_input` with the settings of one solve, and `models.solve_request(request, *,
    constraints=None)`, which returns what `solve` returns for that model. School
    timetabling's `TimetableRequest` holds the problem as the new `TimetableProblemModel`
    with `mode`, `weights` and `initial_solution`, and `solve_request` returns the new
    `TimetableResultModel`; the package now imports its models with itself.
  - *Renamed:* the settings of a request have the names of `solve`'s keywords in every use
    case: `time_limit_seconds` (was `timelimit`), `random_seed`, `parallel_solver_count`
    (was `num_workers`), `stop_at_first_feasible`, and the new `full_assert`, beside
    `solver_parameters`. The shared base is `optimaru._models.SolveRequest`. `timelimit` and
    `num_workers` are still accepted as aliases, in JSON, as keywords and as attributes; a
    dump uses the new names. `model_copy(update=...)` reads the old names too, and refuses a name given with its old name.
  - *Employee scheduling:* `ScheduleRequest` has `random_seed` and `stop_at_first_feasible`
    at the top level like the others; `settings.random_seed` and
    `settings.stop_at_first_feasible` are the same fields under another name (a top-level
    value wins once it is given). Its `solve_request` takes `constraints=`, a list of specs
    that replaces `model_input.constraints` for the solve, and it reads `solver_parameters`
    and `full_assert` too.
- **pydantic 2.11 or newer.** The request models read their earlier field names through
  `validate_by_name` / `validate_by_alias`, which pydantic has from 2.11 (the floor was 2.5).
- **Constraints, not rules.** Employee scheduling called its constraints rules in its API and
  guides; every use case now says constraints. `SchedulingInput.rules` is
  `SchedulingInput.constraints`, `default_rules()` is `default_constraints()`, `Rule`,
  `RuleBase` and `RULE_MODELS` are `ConstraintModel`, `ConstraintModelBase` and
  `CONSTRAINT_MODELS`, `rule_model()` is `constraint_model()`, and `RuleTotal` /
  `ScheduleSolution.rule_totals` are `ConstraintTotal` / `constraint_totals`. The earlier
  names are kept as aliases: `rules` and `rule_totals` are read from JSON and as
  attributes (`rule_totals` read-only), and the old functions and classes are the new ones under a second name.
  *Breaking for JSON consumers:* a dump writes the new names only, so code that reads
  `rules` from a dumped input or `rule_totals` from a dumped solution must read
  `constraints` and `constraint_totals`. The same holds for `timelimit` and `num_workers`
  in a dumped request. The guides and docstrings of the other use cases say "core
  constraints", "coupling constraints" and so on where they said rules for constraints.
- **Late acceptance in stages of growing history.** A late acceptance history is a trade
  between converging fast and finding more, and the use cases set one short length (400 to
  3,200 steps), so a small instance converged within a fraction of a second and the kicks
  alone had the rest of the budget: quadratic assignment, which has no kicks, did nothing
  for the last 29.9 of 30 seconds on 14 facilities. The engine's new
  `AcceptanceSettings::history_growth` (C++; off by default) restarts late acceptance from
  the start's score with the history that many times longer once the search has converged
  and found no new best for half as long as the next stage would take, as long as that
  stage fits in the budget; the best solution is kept as always. It is on, at 2, in **food
  packaging, knapsack, max-cut, quadratic assignment and sports league scheduling**, where
  `late_acceptance_size` is now the first stage's length. Measured on every instance of
  their five benchmark ladders at 30 s over three seeds: 62 runs better, 33 equal, 1 worse
  (a knapsack by 1 in 17,470). In the published tables sports league scheduling has 7 to
  13% less travel, quadratic assignment ties CP-SAT up to 16 facilities (it was behind at
  14 and 16), and the mean gap to the best known values of the 28 QAPLIB instances with a
  reference goes from 2.22% to 0.22%, 18 of them at the best known value. Every other use
  case searches exactly as before. In the five, the ruin & recreate kicks fire differently
  from the first step on: stagnation counts from the last improving move, and a kick waits
  at least two histories. With a time limit, the stage a solve reaches depends on the
  clock; a solve whose step limit ends it before the clock matters repeats for a seed. A
  stage does not know the `unimproved_*` limits: one shorter than a stage ends the solve
  inside it, with the best solution so far. A history stops growing at 2^20 entries (16 MB
  per start).
- **Knapsack: `CoreRepackMove`.** Sorted by value per weight, what the greedy leaves to
  decide sits in a narrow band around the first item the capacity leaves out, and an
  improving exchange there is two items for one or larger, which no flip or swap reaches:
  the 500 to 5,000 item instances of the benchmark stayed 1 to 3 below their optimum. The
  new move (`ks/moves.hpp`, `SolverConfig.core_repack_moves`, on by default) takes a window
  of that order around the break item, frees the capacity its selected items hold and
  fills it with the most valuable subset of the window, a small 0/1 knapsack solved exactly
  by dynamic programming over the weight; the acceptor judges the trade. It looks at the
  capacity only (the score judges resources, groups and dependencies), declines when its
  table would pass a million cells, and is drawn on at most one step in a thousand. All
  seven benchmark instances now end on CP-SAT's value, the four large ones within 0.01 s.
  Parallel starts run the solver's own search (a registered move does not reach the
  engine's multi-start threads).
- **Late acceptance in stages: bed allocation and maintenance scheduling too.** Measured
  on their eleven benchmark instances at 30 s over three seeds. Maintenance scheduling: 13
  of 15 runs better, the mean soft score 24%, 27%, 3.4%, 3.8% and 0.1% better from the
  smallest instance up (the two worse runs: 28 jobs on seed 1, the published one, and 48
  jobs by 2 in 259). Bed allocation: 2.8%, 1.7% and 3.7% better up to 183 stays, the
  20-stay instance on CP-SAT's value on every seed, and within 0.3% either way on the three
  larger instances, where few stages fit. Facility location was measured too and stays as
  it was: its 60-consumer instance got worse on all three seeds.
- **Stages: a restart no longer delays the next one.** The patience before a kick or a
  stage is read from the average gap between new bests, and the gap from the last best of
  one stage to the first of the next spans the whole restart, so after a stage that found
  something the search waited about twice that stage's length before it moved on. That gap
  is now left out of the average. Measured on the six ladders with stages and kicks or
  restarts to gain from, at 30 s over three seeds: sports league scheduling 2.0% less
  travel on average (11 runs better, 4 worse), max-cut 5 better and none worse, bed
  allocation and food packaging level (+0.4%, +0.1%), maintenance scheduling level (6
  better, 5 worse), quadratic assignment the same on every run. The stages are their own
  engine header now, `accept/history_stages.hpp` (`optimaru::HistoryStages`), with unit
  tests of the lengths, the cap and when a stage is due.
- **Benchmarks: the seven tables with stages and QAPLIB measured on this build**
  (2026-11-12). Of their 46 instances the engine has the best score on 45 (18 of them ties)
  and CP-SAT on one: the 14-job maintenance schedule, -3145 against -3133. In 0.4.3,
  before the entries above, CP-SAT had the better score on 14 of the 43 instances those
  tables had.
  QAPLIB: mean gap 0.26% (2.22% before), 17 of 28 at the best known value.
- **Benchmarks: three more rungs, five tables and QAPLIB measured again.** `random25`
  (quadratic assignment), `g1600` (max-cut) and `teams12` (sports league scheduling) join
  their comparison ladders.
- **Docs: adding a constraint, step by step, in every use case.** Each use case has
  `docs/use-cases/<slug>/extending.md` ("Adding a constraint" under its name on the docs
  site). It takes one new constraint through that use case's own files in six steps: change
  a constraint that exists from Python, write a new one as a component, check it, make it
  cheaper per move, put it on the default fast path, measure. Every snippet comes from a patch
  that was compiled and tested, and every figure was measured, on 0.4.3, on the use case's
  demo or a generated instance; each guide says how its two scoring paths differ (which
  constraints the default path scores, what a step is, whether a pick at weight 0 is still
  computed, when `on_reset()` runs) instead of repeating one text. The "Adding a hard or soft
  cost" list in each use case's README is now a pointer to that guide, and
  `docs/extending.md` stays the reference for the contracts. `tests/test_layout.py` requires
  the guide for every use case and holds it to the code: the number of constraints its first
  paragraph states, and every test and C++ test target it names.

## 0.4.3

- **Vehicle routing: visit groups with a partial order.** Visits that share a `group`
  (`VisitModel.group`, a name; `Visit.group_index` on a plan) are one job: they ride one
  vehicle together, or all stay unassigned. `after` (`Visit.after_visit_indices`) lists the
  visits of the same group served before a visit, a partial order on the group; an order
  with a cycle or one that leaves the group is refused. Two new hard constraints at weight
  1000 score it: `visit_group_cost` (the grouped visits not where most of their group is,
  the same counting as pickup-delivery's `request_group_cost`, now the engine's
  `optimaru::GroupPlacement`) and `visit_order_cost` (per vehicle, the pairs served the
  wrong way round). Both answer the move previews. Groups are placed whole before the
  engine's construction, and the use case's first domain move, `groupRuinRecreate`, carries
  a group between vehicles as one unit (`SolverConfig.group_ruin_recreate`,
  `group_ruin_max_visits`); it is registered only for a plan with groups, so a plan without
  them is solved exactly as before. Every entry point scores a grouped input on the
  constraint assembly (`solve_request`, `vrp.solve(plan)`, `Solver.solve(plan)`), on a
  single start, and `calculate_score(plan)` reads a grouped plan the same way (C++:
  `vrp::calculateScore`). Engine-only: the CP-SAT reference reads neither field.
  `list_constraints()` and `constraint_info()` have two more rows.
- **Every acceptance rule has its parameters.** Vehicle routing, quadratic assignment and
  semiconductor scheduling let `SolverConfig.acceptor_type` name seven rules, but each bound the
  parameters of only some of them, so `GREAT_DELUGE` kept its water level at the start's score,
  `STEP_COUNTING_HILL_CLIMBING` ran at a fixed size, and in vehicle routing
  `SIMULATED_ANNEALING` started at temperature 0, which is hill climbing. The three now share
  one set of attributes with the same defaults: `entity_tabu_size` (7),
  `simulated_annealing_starting_temperature` ((0, 0) = automatic) and
  `simulated_annealing_temperature_fraction` (0.01), `great_deluge_water_level_step` ((0, 0)),
  `step_counting_hill_climbing_size` (400), `record_to_record_start_gap` (0.02) and
  `record_to_record_end_gap` (0.0), each checked when it is set. Only one default search
  changes: simulated annealing in vehicle routing, which now starts at 1% of the start's soft
  level (the driving time; the medium level of unassigned visits does not count). In C++ the
  fields are the new `optimaru::AcceptanceConfig` base (`optimaru/solver_config.hpp`), the
  automatic temperature is the engine's (`AcceptanceSettings::start_temperature_fraction`), and
  `optimaru::python::bind_acceptor_type()` binds the enum.
- **Pickup and delivery joins the shared pieces.** Its `SolverConfig` has the acceptance
  attributes above (it had `acceptor_type` and the two record-to-record gaps), so simulated
  annealing no longer starts at temperature 0 there either: it starts at 1% of the start's
  driving time. In C++ its `Score` is the engine's `HardMediumSoftScore`, and its bindings use
  `bind_score()`, `bind_constraints()` and `bind_acceptor_type()`. Its two record-to-record
  gaps are now checked when set (finite and >= 0, a `ValueError` otherwise); nothing else
  changes in Python.
- **Pickup and delivery: a grouped plan is scored the same by every entry point.** The C++
  `Solver::solve(plan)`, and with it Python's `Solver(config).solve(plan)`, scored a plan with
  request groups on the seven core rules, which leave `request_group_cost` out, so a split
  group came back as feasible; only `pdp.solve(plan)` and `solve_request` switched to the
  default constraints. The engine now switches itself (`PdpRoutePlan::hasRequestGroups()`),
  on a single start, and `calculate_score(plan)` (C++: `pdp::calculateScore`) reads a grouped
  plan the same way instead of overwriting its score with the core rules'. As in vehicle
  routing, a request alone in its group changes nothing: such a plan stays on the core rules,
  where 0.4.2 switched for any `group`. With `pair_construction_heuristic` off, a grouped plan
  now starts as any other does there: the engine's construction places every stop, where
  `pdp.solve(plan)` used to leave the caller's unassigned pool for the search to route.
  `request_group_cost` answers the move previews since it counts with
  `optimaru::GroupPlacement`, and answers them piecewise for a plan without groups; the
  default set still applies every move, because its three request-coupling constraints do
  not preview.
- **Pickup and delivery notebook re-run on 0.4.2.** `examples/pickup-delivery.ipynb` is saved
  with outputs from optimaru 0.4.2: the same problem now ends at 5.2 hours of driving, where
  0.4.1 ended at 5.4. Its rounds of more search are 10,000 steps each (were 0.3 seconds),
  because 0.4.2 settles within one 0.3-second round and both lines of the chart stayed flat.
- **Benchmarks: the projects pin external-solver `v0.2.3`.** That release lists the engine's
  new `request_group_cost` among the pickup-delivery constraints the CP-SAT model has no
  plugin for (selecting it there is an unknown-constraint error, as with
  `required_vehicle_cost`); its models are those of `v0.2.2`, so no published result moves.
  The benchmark projects' lock files record optimaru 0.4.2. Six of the projects (conference
  scheduling, food packaging, meeting scheduling, order picking, sports league scheduling,
  task assigning) had no `[build-system]` table, so uv did not install them and the commands
  their READMEs give (`uv run <slug>-compare`, `uv run pytest -q`) did not run; they have
  the table the other fifteen have now, and declare pytest in a `dev` dependency group, so
  a plain `uv sync` installs it. `tests/test_layout.py` holds every benchmark project to a
  `[build-system]` table and to one external-solver tag, and the links into external-solver
  to that tag.
- **Layout and names: one form for what every use case has.** No behavior changes.
  - The C++ quality benches left the package tree: `optimaru/<use case>/cpp/bench/` is now
    `benchmarks/<slug>/cpp/<ns>_quality_bench.cpp` (bed allocation, conference scheduling,
    meeting scheduling, school timetabling; conference scheduling's file was
    `quality_bench.cpp`), next to the comparison it belongs to, as the C++ tests are under
    `tests/<use case>/cpp/`. The bench targets and their binaries' paths under `build/` are
    the same.
    A use case's `cpp/` holds only `CMakeLists.txt`, `bindings.cpp`, `include/` and `src/`.
  - School timetabling's C++ tests carry the use case's prefix like every other target:
    `st_canonical_test`, `st_canonical_plugin_test` (were `canonical_test`,
    `canonical_plugin_test`; files, targets and ctest names).
  - Semiconductor scheduling's tests use the names the other use cases' tests use:
    `fixtures/web_demo.json` (was `data/web-demo.json`) and `cpp/fab_fixtures.hpp` (was
    `fab_test_support.hpp`).
  - Three benchmark projects are named after their use case like the other seventeen
    that run an external-solver model:
    `flight_crew_scheduling_cpsat`, `project_job_scheduling_cpsat` and
    `sports_league_scheduling_cpsat` (were `flight_crew_cpsat`, `project_job_cpsat`,
    `sports_league_cpsat`), with the commands `flight-crew-scheduling-compare`,
    `project-job-scheduling-compare`, `project-job-scheduling-public-bench` and
    `sports-league-scheduling-compare`. `scripts/publish_benchmarks.py` no longer keeps a
    table of exceptions for them.
  - Every benchmark project declares pytest in the `dev` dependency group. Ten declared it
    as an extra and school timetabling's CP-SAT project in a group named `test`, so a plain
    `uv sync` there installed no pytest and `uv run pytest -q` could not start. The READMEs
    and CI now say `uv sync` without `--all-groups --all-extras` / `--extra dev`.
  - Guide titles: the use case's name in sentence case, without "(C++ / Python)"; every
    `python.md` is `# optimaru.<use case> (Python)`; six `cpsat/README.md` titles follow the
    other nine (`<Use case> CP-SAT`).
  - `tests/test_layout.py` holds the `cpp/` contents and the `dev` group.

## 0.4.2

- **Pickup and delivery: request groups.** Requests that share a `group`
  (`DeliveryModel.group`, a name; `Delivery.group_index` on a plan) are one job: they ride
  one vehicle together, or all stay unassigned. Their stops stay ordinary stops, visited in
  any order between other requests' stops. The new constraint `request_group_cost` (hard,
  weight 1000, flippable) counts the grouped requests that are not where most of their
  group is. Construction and the request moves place a group as one unit, each request at
  its cheapest slot pair once the earlier ones are in, and leave all of it unassigned when
  no single vehicle has room; the pair move, the pair exchange and the pair ruin stay
  inside a group. A `required_vehicle` on one request, a pinned stop or a request already
  on an initial route binds the whole group to that vehicle, and two requests of a group
  bound to different vehicles (by a required vehicle or a pinned stop) are refused.
  `solve_request` scores groups whenever a request names one, and so does `pdp.solve(plan)`
  for a plan with a `group_index` set (both then score the whole default set). Engine-only:
  the CP-SAT reference does not read `group`. `list_constraints()` and `constraint_info()`
  have one more row, before `unassigned_requests_cost`. A plan without groups is solved
  exactly as before.
- **Engine: `MemberChangeGlobalConstraint`.** A global constraint that keeps its aggregates
  from joins and leaves alone derives from `optimaru::MemberChangeGlobalConstraint<Solution>`
  (`optimaru/score/decomposed_objective.hpp`) and writes `on_member_changed()` only; ten use
  cases declared that base themselves. The comment that opens each use case's
  `constraints/common.hpp` now holds what is particular to the use case and points to the
  engine header and `docs/extending.md` for the rest. No score changes.
- **Bindings: the part every use case repeated is two engine helpers.**
  `optimaru::python::bind_constraints()` binds `constraint_costs()`, `list_constraints()` and
  `constraint_info()` off a default assembly, and `bind_score()`
  (`optimaru/python/score_binding.hpp`) binds the `Score` class of the engine's two score
  types. The Python functions, their arguments and their results are the same; the
  docstrings of `_core.constraint_costs` and `_core.list_constraints` are now the same text
  in every use case.
- **Engine: the three-level score is one type.** `optimaru::HardMediumSoftScore`
  (`optimaru/score/hard_medium_soft_score.hpp`) replaces the identical `Score` structs that
  bed allocation, food packaging, project job scheduling and vehicle routing each declared,
  with `to_engine_score()`, `to_engine_score_limit()` and `from_engine_score()` for the fold
  into the engine's two levels. Each use case's `Score`, `toEngineScore()` and
  `fromEngineScore()` keep their names and results. `kNoLevelFloor` moves to
  `optimaru/score/levels.hpp`. Nothing changes in Python.
- **Pickup and delivery: the search works in whole requests.** Three new move operators take
  a pickup and its dropoff out of a route together and put them back together, at the
  cheapest feasible slot pair, so the acceptor is offered plans with every request whole
  instead of split ones to reject. `requestRuinRecreate` takes 1 to
  `request_ruin_max_requests` requests out (random, related, the costliest, one route's, or
  adjacent stretches of nearby routes) and reinserts them in random order or by regret, with
  blinking; requests waiting unassigned join the reinsertion. `bundleRelocate` moves up to
  `bundle_max_requests` requests whose pickups follow each other on a route to one other
  route, and `bundleExchange` swaps two such bundles. A slot pair is checked in O(1) from a
  per-route timeline (departures, latest feasible arrivals, loads), against the capacity,
  the time windows and the vehicle limits that are set (`max_end_time`,
  `max_route_duration`, `max_travel_time`, `max_stops`, pinned stops, a required vehicle).
  Construction uses the same scan, so the first plan keeps those vehicle limits too (it
  checked capacity and time windows only). `SolverConfig` gains `request_ruin_recreate`,
  `request_ruin_max_requests` (20), `bundle_moves` and `bundle_max_requests` (3); the two
  switches are on by default, so a seeded solve no longer walks the path it did, and the
  two counts refuse a value below 1. `pdp_demo`'s control flag is `--stop-moves-only` now
  (it was `--no-pair-moves`) and turns every request-level operator off. Both results
  blocks of `benchmarks/pickup-delivery` are measured again (30 s, seed 0). On the 56 Li &
  Lim instances the engine's mean distance goes from 7.5% above the best known to 0.8%
  below it (the harness minimizes distance only, so a plan with more vehicles can be
  shorter than the best known), the worst instance from +22.0% to +2.4%, the instances
  more than 1% above from 43 to 2, and the vehicles used from 485 to 409 (best known:
  402). The generated instances of 25 to 400 requests come out 2% to 10% shorter.
- **Engine (C++): features no use case used are removed.** Gone from the engine headers: the
  chain solution shape (`Shape::Chain`, `ChainObjective`, the chain overload of
  `Solver::solve()`, the `chain::` link helpers), `WeightedObjective`, `explain_score()`,
  `rescore()` and `ScoreBreakdown`, background solves (`solve_in_background()`,
  `BackgroundSolve`) and derived state (`DerivedState`, `Solver::add_derived_state()`, the
  fourth argument of `Editor`). None of the 22 use cases used any of them, so the Python
  package does not change, and a seeded, step-limited solve gives the same result as before.
  What a C++ caller uses instead: `DecomposedObjective::constraint_totals()` for the score
  per constraint, and `Solver::solve()` on a thread of its own, stopped with
  `request_stop()`, for a solve in the background.
- **School timetabling benchmark: the CP-SAT package keeps only what the benchmark runs.**
  `benchmarks/school-timetabling/cpsat` still carried parts of the application it came from
  that no benchmark and no test of the model used: a workbook importer, timetable and
  school-system exports, a timetable inspection report, a sample workbook, and settings read
  from environment variables. They are removed (about 2,250 lines) with the two dependencies
  only they needed (`openpyxl`, `pydantic-settings`). The model, its constraints and the three
  benchmark tracks are unchanged.
- **More instances in every engine-vs-CP-SAT comparison.** Each of the 17
  comparisons on generated or demo instances now runs a ladder of five to
  seven instances of growing size, 99 in all (was 38), and every results
  block is measured again. The smallest rungs are ones both solvers finish;
  on the largest the exact model often returns nothing within the budget,
  and the tables say so. `benchmarks/README.md` now states how a CP-SAT run
  is timed: the 60 s stop past the budget, and the level-by-level solve of
  the models whose score does not fit one 64-bit objective.
  - Conference scheduling, maintenance scheduling, meeting scheduling, sports
    league scheduling and task assigning had only their demo data. Their
    `benchmark.py` has a seeded generator in the shape of that data, and the
    demo instance stays as one rung. Food packaging runs its demo generator at
    six sizes.
  - The benchmark projects pin external-solver `v0.2.2` (was `v0.1.0`; the
    models are the same). Its level-by-level solve used to end without a
    solution when the first level found none in its share of the budget (10 s
    of 30 s with three levels); the first level now keeps the time that is
    left. Two rungs whose whole result was that early end, the 4-line plant of
    food packaging and the 240-task instance of task assigning, are back in
    their ladders, and the five comparisons that use the level-by-level solve
    (conference scheduling, food packaging, meeting scheduling, task assigning,
    tournament scheduling) were measured on that release.
  - The comparison runner takes `--solver engine` or `--solver cp-sat`, so the
    single-threaded engine runs and the CP-SAT runs can be measured side by
    side. The publisher joins the two files (`compare/<slug>.engine.json`,
    `compare/<slug>.cp-sat.json`) after checking that they belong together,
    and refuses a comparison that lacks a run of either solver. Every result
    records its seed and workers.
  - `pickup-delivery-public-bench` and `project-job-public-bench` take `--out
    FILE` and rewrite it after every instance, so an interrupted sweep keeps
    what it measured.
  - The publisher refuses a routing file that was run on chosen instances
    (`--only`) or whose two series used different seeds, and publishes a
    pickup-delivery file of one backend with the other as not run.
  - The SALBP details workbook must name its columns: a missing header is an
    error, no longer a fallback to fixed columns.
- **A parallel start that reaches the target stops the others.** With
  `parallel_solver_count` above 1 and a target (`stop_at_first_feasible`, `best_score_limit`),
  each start used to stop on its own, so `solve()` returned only when the slowest start had
  reached the target or run out the time limit. Now the first start to reach it ends the run:
  the others stop at their next step and hand over their best so far, and the best of all
  starts is returned. That solution can score lower on the levels below the target than
  the best of all starts run to the end did, which is what the earlier return costs. Which
  start gets there first depends on the threads, so in that combination the same seed can
  give a different solution from run to run; without a target, or with one start, a seeded
  solve is as reproducible as before. C++: the same holds for
  `Solver::solve_multistart()` and `optimaru::solve_parallel_starts()`.
- **Benchmarks on whole public data sets.** The public-instance harnesses now run
  whole sets or stated samples instead of a few instances, and every results
  block is measured again on 0.4.1: CVRPLIB Set X (all 100) and Augerat's Set A
  (all 27), the Li & Lim PDPTW set (all 56, beside the best-known solutions),
  RG30 Pat1–Pat5, Falkenauer bin packing (three per class), QAPLIB (the 33
  instances with 30 to 50 facilities) and SALBP-1 (five per data set).
  `scripts/publish_benchmarks.py` writes those tables and one homepage entry per
  data set. What changed in the harnesses:
  - *Vehicle routing.* Set X does not fix the fleet, and the best-known solutions
    of 22 of its instances use more routes than the `-kN` of their name, which the
    harness used as the fleet. Every backend now gets a fleet that cannot bind on
    Set X (30% above the capacity lower bound, plus three); Set A keeps `-kN`.
    `--series a` runs Set A and `--only NAME` picks instances of a series.
  - *Pickup and delivery.* One Li & Lim unit is ten seconds instead of one, so
    travel is rounded to a tenth of a unit and a request of the `lr` and `lrc`
    classes can no longer be left out for free (their 10-unit service time was
    under a whole service minute). `--instance`, `--requests` and `--synthetic`
    repeat, `--all` runs the archive (whole instances unless `--requests` is
    given), `--json` prints the results with each run's outcome, and the engine
    runs single-threaded whatever `--workers` says, as the READMEs stated.
  - *Project scheduling.* The RG30 archive is checked by the SHA-256 of its five
    instance files: the archive itself is rebuilt upstream, and the pinned archive
    checksum no longer matched. `--all`, a repeatable `--instance` and `--json`
    are new, and the engine runs single-threaded there too.
  - *Quadratic assignment.* `--all` stopped at `kra30a`: the harness re-scores
    each QAPLIB reference permutation and read every solution file as the
    facility of each location, while four files list the location of each
    facility. Both readings are tried. `kra32`'s file states 88900 while its
    permutation scores 88700, which is recorded as an erratum; any other
    mismatch still stops the run.
  - *Bin packing.* `--sample-per-class N` picks N evenly spread instances of each
    Falkenauer class.
  - *Assembly line balancing.* The best-known values of the medium (n=50) data
    sets were read from the wrong columns of their details workbook, which has
    one more column; the columns are now found by their headers.
- **Meeting scheduling: `parallel_solver_count` runs that many starts.** It was ignored: the
  solver set the engine's start count and then called the engine's single-start entry point,
  so `solve(..., parallel_solver_count=4)` searched once, on one thread. It now runs the
  engine's multi-start, as the other use cases do: start *i* searches with `random_seed + i`
  and the best score wins. A solve with one start, and one with `constraints=...` (always
  single-start), returns what it returned before.
- **C++: every `Solver::Config` field is snake_case, and the common ones come from a shared
  base (breaking for C++ callers).** A `Config` mixed `time_limit` and `seed` with
  `parallelSolverCount`, `ruinRecreateMaxRuinCount` and `nearbyProbability`; every camelCase
  field is now snake_case (`parallel_solver_count`, `ruin_recreate_max_ruin_count`,
  `nearby_probability`, ...). The fields each of the 22 use cases declared for itself (the
  limits, the seed, the parallel starts, the score checks, `on_new_best`, and the ruin &
  recreate knobs) are declared once, in `optimaru::SolverConfigBase<Score>` and
  `optimaru::RuinRecreateConfig` of the new `optimaru/solver_config.hpp`, which also holds
  what every solver did with them: `check_config()`, `settings_from()` and
  `solve_parallel_starts()`. A use case's `Config` derives from the bases and declares only
  its own knobs. Python is not affected: every `SolverConfig` keeps its attributes and their
  defaults, and a seeded solve walks the same path.
- **Maintenance scheduling: `SolverConfig` gains `ruin_recreate_as_move_weight` and
  `ruin_recreate_as_move_max_ruin`.** The C++ solver already had the two knobs
  (destroy-and-repair as an ordinary move judged by the acceptor; 0, the default, turns it
  off), but Python could not set them.
- **Semiconductor scheduling: a wrong problem type is a `TypeError`.** `solve()`,
  `calculate_score()`, `constraint_costs()` and `constraint_items()` raised `AttributeError`
  for a value that was neither a `SemiconductorSchedule` nor a
  `models.SemiconductorScheduleModel`; they now say which two types they take, as the other
  use cases do.
- **Flight crew scheduling ignores `FCS_*` environment variables.** Three overrides a
  tuning session left in the solver (`FCS_DESCENT_WEIGHT`, `FCS_ASMOVE_WEIGHT`,
  `FCS_ASMOVE_MAXRUIN`) are gone.
- **Engine (C++): unused settings removed.** `AcceptanceSettings::rrt_steps`,
  `rrt_exponential` and `rrt_compare_to_best` were set by nothing; record-to-record travel
  keeps the schedule it always ran, linear against the best score. `Solver::clear_stop()`
  is gone too: a stopped solver stays stopped.
- **Notebooks.** The notebooks in `examples/` are saved with outputs from optimaru 0.4.1 again.
  The conference-scheduling notebook read a `hard` key that `constraint_costs()` no longer
  returns and did not run; a few sentences that named a result the outputs no longer show are
  reworded, and the food-packaging rounds use 50,000 steps.
- **Importing a use case imports its models.** In 15 use cases `import optimaru.<use case>`
  left `optimaru.<use case>.models`, and with it pydantic, for the first call that needed a
  model. They now import it with the package, as `assembly_line_balancing`, `bin_packing`,
  `knapsack`, `max_cut`, `pickup_delivery` and `quadratic_assignment` already did (school
  timetabling, whose `solve()` takes dicts, still does not). pydantic is a dependency of
  `optimaru`, so nothing new has to be installed; the first import of one of the 15 takes
  about 35 ms longer (`vehicle_routing` 15 ms to 48 ms, `employee_scheduling` 25 ms to
  72 ms, measured with `python -X importtime` on one machine), which is the time the first
  model call used to take. On the package,
  `with_initial_solution` (and `vehicle_routing.solve_request`) is now the `models` function
  itself, with its docstring and annotations, instead of a wrapper around it.
- **A star import of five modules binds their own names only.** `models` in `knapsack`,
  `max_cut`, `quadratic_assignment` and `semiconductor_scheduling`, and
  `employee_scheduling.helpers`, had no `__all__`, so `from ... import *` also bound the names
  they import (pydantic's `BaseModel` and `Field`, for example). They now list their own
  classes and functions, as the other modules do. Every `__all__` in the package is sorted.

## 0.4.1

- **One `solve()` for every use case (breaking for seven).**
  `optimaru.<use case>.solve(problem, ...)` now exists in all 22, takes the same
  keywords (`time_limit_seconds`, `random_seed`, `parallel_solver_count`,
  `stop_at_first_feasible`, `full_assert`, `on_new_best`, `config`,
  `constraints`) and returns the solved problem, with the score on it.
  - *Breaking:* in `assembly_line_balancing`, `bin_packing`,
    `facility_location`, `knapsack`, `max_cut`, `quadratic_assignment` and
    `semiconductor_scheduling`, `solve()` returned the `Score`. It now returns
    the problem it was given: write `uc.solve(problem).score` for a compiled
    problem, or read `score_hard` / `score_soft` from the solved pydantic
    model.
  - *Breaking:* given a pydantic model, `solve()` in
    `assembly_line_balancing`, `knapsack`, `max_cut`, `quadratic_assignment`
    and `semiconductor_scheduling` wrote the result into that model. It now
    returns a new solved model and leaves the one passed in as it was, as the
    other use cases do: write `model = uc.solve(model)`. Code that keeps
    reading the model it passed in does not fail, it reads the unsolved
    values. (`calculate_score(model)` still fills in the score fields of the
    model it is given.)
  - *New:* `solve()` in `bin_packing`, `conference_scheduling`,
    `facility_location`, `food_packaging`, `maintenance_scheduling` and
    `sports_league_scheduling` also takes the use case's pydantic problem
    model and returns a solved model, as the others do. (Employee scheduling,
    order picking, pickup & delivery and vehicle routing keep
    `models.solve_request` for their request models.)
  - *New:* `solve()` in `employee_scheduling`, `pickup_delivery` and
    `vehicle_routing`; the `full_assert` keyword in the 15 use cases whose
    `solve()` lacked it; `SolverConfig.full_assert` in those three and
    `SolverConfig.parallel_solver_count` in `employee_scheduling`.
  - *Stricter:* `SolverConfig` in `pickup_delivery` and `vehicle_routing`
    rejects `parallel_solver_count <= 0` and `late_acceptance_size < 0`, and in
    those two and `order_picking` a `ruin_recreate_rewards` list that does not
    hold exactly 4 values, with the `ValueError` the other use cases raise.
- **Engine: repeated instances in `ConstraintAssembly`.** A by-name edit or
  lookup (`weight()`, `level()`, `hard()`, `soft()`, `exponent()`, `offset()`,
  `clip()`, `find()`, `drop()`, and `add()`'s replacement) of a name the
  assembly holds several times now throws `std::invalid_argument`. It used to
  touch the first instance only, so `weight("x", 0)` after `add_instance()`
  left the second instance in the score. New overloads take the instance,
  counted from 0: `weight(name, instance, w)` and the same for the other
  setters, `find(name, instance)` and `drop(name, instance)`.
  `instance_count(name)` says how many there are and `drop_all(name)` removes
  them all. `find(name)` throws for such a name as well, where it used to
  return the first instance; `contains(name)` never throws. A name held once
  behaves as before, and so does the Python API,
  which never edits a repeated name.
- **Engine: repeated picks share the raw cost.** The second and later picks
  of a constraint used to run its cost and delta code once more each. They
  now join as repeats (`ConstraintAssembly::add_repeats()`, which the Python
  bindings use), and `DecomposedObjective` asks one instance of a unit
  constraint per unit and shapes the answer for each pick. Scores do not
  change. A vehicle-routing solve with `TravelTimeCost()` and
  `TravelTimeCost(exponent=2)` took about a quarter more steps per second, and
  one with a third pick at `exponent=3` about half more (400 visits). Picks
  are still computed separately when the engine handles them differently (one with a negative
  threshold next to one without), for constraints with domain parameters
  (employee scheduling), for the members of a compiled delta group, and for
  global constraints. `add_instances()` keeps independent instances.
- **Docs: the cost of a negative threshold.** The guides now say that a
  negative `offset` with `clip=True` (a fixed charge per non-zero unit) is
  recomputed for every unit a move changes, which about halves the steps per
  second of a vehicle-routing solve.
- **Docs and messages.** The guides, docstrings and comments describe each use
  case in its own terms and no longer compare it with the examples it was
  derived from. Two `optimaru.project_job_scheduling` error messages changed
  wording with them (a non-renewable global resource, a standard job with no
  execution mode). No behaviour changes.
- **Repository layout.** The source tree now has the shape of the package it
  builds. `optimaru/` is the one Python package. The engine headers moved from
  `include/optimaru/` to `optimaru/cpp/optimaru/` (the include path stays
  `#include "optimaru/..."`), and each use case moved from
  `use-cases/<slug>/{cpp,python}` into its subpackage `optimaru/<use case>/`,
  with its C++ in `optimaru/<use case>/cpp/`. Tests moved to `tests/` (engine,
  shared constraint base, one directory per use case), and the guides to
  `docs/`. The standalone per-use-case packages (`vrp_routing`,
  `st_timetabling`, ...) and their 22 copies of the constraint-spec base are
  gone: everything imports `optimaru.<use case>`. `setup.py` finds the use cases
  and their sources itself. The installed package and its
  API do not change.
- **License.** A new `THIRD-PARTY-LICENSES` file ships in the wheels and the
  sdist. It carries the notices and license texts of the C++ runtime libraries
  that the wheel repair tools bundle into the musllinux wheels (libstdc++ and
  libgcc, GPL-3.0-or-later with the GCC Runtime Library Exception) and the
  Windows wheels (the Microsoft Visual C++ runtime). The 0.4.0 wheels bundle the
  same libraries without it.
- **Builds.** One `uv sync` at the root builds every extension in place, and
  one CMake project (`cmake -B build`) builds the engine and every use case's
  C++ tests. `OPTIMARU_USE_CASES` narrows either build to some use cases.
- **Docs.** `docs/extending.md` is a step-by-step guide to adding a move or a
  cost, and a documentation site (MkDocs, with an API reference generated from
  the sources) is built from `docs/` and published on GitHub Pages. The engine
  layout in the README lists every directory. `CONTRIBUTING.md` and a section
  near the top of the README give the short version of adding a constraint or
  a move and link to the guide.
- **Bindings.** The part of `SolverConfig` that every use case shares (the
  limits, the seed, `parallel_solver_count`, `late_acceptance_size`,
  `full_assert`, `ruin_recreate_rewards`) is bound by one helper,
  `optimaru::python::bind_solver_config`, instead of 22 copies. Names, defaults
  and error messages are unchanged.
- **Engine internals.** The step loop (`search/local_search.hpp`) and the
  decomposed objective (`score/decomposed_objective.hpp`) are shorter: the
  move mix, the kick's polish loop and the adaptive selectors share one
  sampling and scoring path, the score levels fold in one place, and the
  built-in per-constraint profiler (the undocumented `ES_PROFILE` environment
  variable) is gone; use a sampling profiler instead. A seeded, step-limited
  solve gives the same result as before. Two source-incompatible changes for
  C++ code that includes the engine headers: `ListMove::Kind` enumerators are
  PascalCase like every other engine enum (`Kind::Segment`, `Kind::TailSwap`,
  ...), and `GreedyRepair::candidate_count` is replaced by
  `ruin_candidate_count`.
- **Initial solutions.** `with_initial_solution` in
  `optimaru.flight_crew_scheduling` and `optimaru.project_job_scheduling`
  raises `ValueError` for a problem that repeats an id, like the other use
  cases.

## 0.4.0

First release.

- **Python package.** `pip install optimaru` installs one package with every
  use case as a subpackage (`optimaru.vehicle_routing`,
  `optimaru.school_timetabling`, ...), as prebuilt wheels for Linux, macOS and
  Windows on Python 3.11 and later. No C++ toolchain is needed.
- **Engine.** A header-only C++17 metaheuristic engine under
  `include/optimaru/`, with no dependencies and no domain code:
  - three solution shapes (assignment, sequence and chain) and derived state
    kept up to date around every write;
  - incremental scoring with `Score` (int64 hard, double soft), written as an
    `Objective` or summed from named components (`WeightedObjective`,
    `DecomposedObjective`);
  - construction heuristics (first fit, first fit decreasing, cheapest
    insertion), acceptors (hill climbing, late acceptance, entity tabu,
    simulated annealing, great deluge, step counting hill climbing,
    record-to-record travel) and assignment and sequence moves mixed by weight;
  - nearby selection and adaptive ruin and recreate (random, related, worst,
    SISR and route ruin with regret-k recreate);
  - stop conditions on wall time, stall time, steps, stall steps and a target
    score; multi-start solves and background solves (`solve_in_background()`)
    that report status, stop on request and rethrow a failure from `wait()`;
  - score explanation (`explain_score()`, `rescore()`) and a full-check mode
    (`ScoreChecks::Full`) that compares every incremental score with a full
    recompute.
- **22 use cases**, each a C++ domain with a pybind11 Python package, tests and
  a README: assembly-line balancing, bed allocation, bin packing, conference
  scheduling, employee scheduling, facility location, flight crew scheduling,
  food packaging, knapsack, maintenance scheduling, max-cut, meeting
  scheduling, order picking, pickup and delivery, project-job scheduling,
  quadratic assignment, school timetabling, semiconductor scheduling, sports
  league scheduling, task assigning, tournament scheduling and vehicle routing.
  Each package builds and tests on its own from `use-cases/<slug>/python`.
- **Constraint contract**, the same in every use case:
  - every constraint is one named component (`<Subject>Cost`, Python name
    `<subject>_cost`) whose raw cost is 0 when nothing is violated;
  - `constraint_info()` lists the constraints with their default weight and
    level; `constraint_costs()` returns the raw per-unit costs of a solution,
    and the `constraints.score()` helper re-prices them under any selection
    without solving again;
  - each pick takes five knobs: `weight`, `level` (`"hard"`, `"medium"` where
    the use case has a medium level, or `"soft"`), `exponent`, and `offset` /
    `clip`, which shift the shaping per unit as a threshold
    (`max(0, x - offset) ** exponent`) or as the literal
    `(x - offset) ** exponent`;
  - a constraint may be picked several times, each instance with its own knobs
    and parameters; the instances add up;
  - guards keep scores meaningful: rewards never go hard, the literal offset
    formula and raw costs that can be negative stay off the levels that count
    toward the hard score, structural constraints keep their level, and the
    hard total saturates instead of wrapping.
- **Examples.** One Jupyter notebook per use case in `examples/`, with
  visualizations.
- **Benchmarks.** Every use case but vehicle routing is compared with OR-Tools
  CP-SAT on the same instances and rescored by the use case's own evaluator;
  vehicle routing is compared with the OR-Tools Routing solver on CVRPLIB Set X.
  The CP-SAT models come from the separate
  [external-solver](https://github.com/optimaru/external-solver) repository,
  except school timetabling's, which stays in `benchmarks/`. Pickup and
  delivery, project-job scheduling, bin packing, quadratic assignment,
  assembly-line balancing and vehicle routing also run on public instance sets
  (Li & Lim, RG30, Falkenauer, QAPLIB, SALBP, Augerat), downloaded on demand as
  SHA-256 pinned files.
- **License.** Apache License 2.0. NOTICE states that the engine is a modified
  derivative of Timefold Solver (itself derived from OptaPlanner and OptaPy),
  lists the use cases and benchmark harnesses derived from Timefold
  Quickstarts, and includes pybind11's BSD-3-Clause license, which the
  compiled extensions embed.
