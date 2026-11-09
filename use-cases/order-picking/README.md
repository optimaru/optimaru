# Order Picking

Derived from the Timefold Quickstarts `order-picking` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../NOTICE).

Domain model, constraints and API details: [`python/`](python/README.md) and [`cpp/`](cpp/README.md).

## Constraints (Score — the engine score type as-is, no mapping)

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost per trolley (0 = no violation) |
|---|---|---|---|---|
| `trolley_capacity_cost` | `TrolleyCapacityCost` | hard | 1 | buckets short: `max(0, sum over its orders of ceil(order volume / bucket capacity) - bucket count)` (orders never share a bucket) |
| `distance_from_previous_pick_task_cost` | `DistanceFromPreviousPickTaskCost` | soft | 1 | warehouse distance (m) start -> first pick -> ... -> last pick |
| `distance_from_last_pick_task_to_path_origin_cost` | `DistanceFromLastPickTaskToPathOriginCost` | soft | 1 | warehouse distance (m) from the last pick back to the start; 0 for an empty trolley |
| `order_split_by_trolley_cost` | `OrderSplitByTrolleyCost` | soft | 1000 | distinct orders with a pick on the trolley — summed, the (order, trolley) pairs. **Not** 0 for an unsplit order: an order on one trolley counts 1, so the minimum is the number of orders with assigned picks (the Java quickstart's definition, kept) |

Every constraint is flippable (`level="hard"` / `level="soft"`, or `Level.HARD` /
`Level.SOFT`; `level="medium"` raises `ValueError`, the domain has no medium level),
none is a reward and all four are scored by default (`default_enabled`);
`order_picking.constraint_info()` reports exactly this table from the C++ components (`kDefaultLevel` /
`kDefaultWeight`). `order_picking.constraint_costs(solution)` is the only
per-constraint surface: the travel distance is the sum of the two distance entries,
the (order, trolley) pair count the sum of `order_split_by_trolley_cost`
(`models.solve_request` fills `total_distance` / `order_split_count` from it).

`order_split_by_trolley_cost`'s raw cost is the (order, trolley) pair count; its default
weight is 1000.

Besides `weight`, `level` and `exponent`, every constraint takes `offset` and `clip`
(keyword-only), and may be picked several times. Each trolley's raw cost `x` is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which lowers the penalty of
every trolley below the offset, an empty one included); `offset=0` is the plain shaping.
Both apply per trolley, not to the total. `clip=False` with an offset is refused on hard
(`trolley_capacity_cost` by default), because hard 0 must mean no violation. Each pick is
its own instance with its own knobs, and the instances add up:

```python
from order_picking.constraints import (
    DistanceFromLastPickTaskToPathOriginCost, DistanceFromPreviousPickTaskCost,
    OrderSplitByTrolleyCost, TrolleyCapacityCost,
)

op.solve(solution, constraints=[
    TrolleyCapacityCost(),
    DistanceFromPreviousPickTaskCost(),                                     # meters walked
    DistanceFromPreviousPickTaskCost(weight=0.1, exponent=2, offset=100),   # + 0.1 x (meters beyond 100)^2
    DistanceFromLastPickTaskToPathOriginCost(),
    OrderSplitByTrolleyCost(),
])
```

`op.constraint_costs(solution, picks)` keys the second pick
`distance_from_previous_pick_task_cost#2`. The CP-SAT reference takes integral offsets.

## Usage

```sh
cd use-cases/order-picking/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import order_picking as op

solution = op.build_demo_data()
solution = op.solve(solution, time_limit_seconds=5.0, random_seed=0)
print(solution.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`order_picking.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/order-picking/cpp -B build/order-picking && cmake --build build/order-picking -j
ctest --test-dir build/order-picking
```

## Starting from an existing solution

```python
from order_picking.models import solve_request, with_initial_solution

solution = solve_request(request)                        # request: an OrderPickingRequest
next_request = with_initial_solution(request, solution)  # or solution.model_dump(mode="json")
better = solve_request(next_request)
```

`with_initial_solution` returns a new request whose trolleys carry `initial_route`, a list of pick task ids. The solve keeps those routes as its starting point and construction places only the tasks in no route, so a partial solution works. With `timelimit=0` a complete solution comes back as it went in. An unknown id or a task routed twice raises `ValueError`.

A pick task id is `<order id>-<item position>`, so keep each order's `items` in the order they were solved in: moved, the same id names a different item.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The solution has the **sequence** shape; the domain solver that configures the
engine is `cpp/include/op/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). `OrderConsolidateMove` (pulls an order's items onto one trolley) is defined in `cpp/src/solver.cpp` and registered there when `orderConsolidateMoves` is on — use it as the template. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** Elements are placed by cheapest insertion in index order. Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/op/constraints/` (currently `trolley_capacity_cost`,
`distance_from_previous_pick_task_cost`, `distance_from_last_pick_task_to_path_origin_cost`,
`order_split_by_trolley_cost`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/op/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` (`kHardWeight<X>` / `kSoftWeight<X>` in `constraints/common.hpp`) —
   `op_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `op_test` / `op_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/order_picking/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=[..., MyRuleCost(weight=5, level="hard")]`;
5. mirror it in the CP-SAT model in
   [`benchmarks/order-picking/cpsat`](../../benchmarks/order-picking/cpsat) (a plugin
   subclassing the spec; defaults come from `constraint_info()`) so the benchmark stays like
   for like.

## Benchmark

[`benchmarks/order-picking`](../../benchmarks/order-picking) compares this use case with OR-Tools CP-SAT on the same instances.
