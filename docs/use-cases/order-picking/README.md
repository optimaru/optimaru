# Order picking

Derived from the Timefold Quickstarts `order-picking` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../../NOTICE).

Domain model, constraints and API details: [`python.md`](python.md) and [`optimaru/order_picking/cpp/`](cpp.md).

## Usage

```sh
OPTIMARU_USE_CASES=order_picking uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/order_picking
```

```python
from optimaru import order_picking as op

solution = op.generate_demo_data()
solution = op.solve(solution, time_limit_seconds=5.0, random_seed=0)
print(solution.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`order_picking.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=order_picking && cmake --build build -j
ctest --test-dir build
```

## Warehouse layout

The layout is input. A request, or a compiled solution, uses one of two kinds; without
either it has the default grid, and solves as it did before the layout was input.

**A grid of shelvings.** Columns `A`, `B`, ... by shelving rows `1`, `2`, ...; a location is a
shelving, its `LEFT` or `RIGHT` side and a row along that side, in meters from the shelving's
top edge. `warehouse` on the request sizes it:

| `warehouse` field | Default | Range | Meaning |
| --- | --- | --- | --- |
| `column_count` | 5 | 1..18278 | shelving columns, `A`..`E` by default |
| `shelving_row_count` | 3 | 1..10000 | shelving rows, from 1 |
| `shelving_width` | 2 | 1..1000 | meters between the two sides of a shelving |
| `shelving_height` | 10 | 1..1000 | meters along a side; a side has the rows `0`..`shelving_height` |
| `shelving_padding` | 3 | 1..1000 | meters of corridor between neighboring shelvings, both ways |

A location's `column` is written in letters as a spreadsheet writes its columns: `A`..`Z`, then
`AA`, `AB`, ... up to `ZZZ`. A location outside the grid is refused, naming the trolley or the
order item.

```python
from optimaru.order_picking.models import (
    LocationModel, OrderModel, OrderPickingRequest, ProductModel, TrolleyModel, WarehouseModel, solve_request,
)

request = OrderPickingRequest(
    time_limit_seconds=2,
    warehouse=WarehouseModel(column_count=30, shelving_row_count=4, shelving_height=20),
    trolleys=[TrolleyModel(id="1", bucket_count=4, bucket_capacity=48000,
                           location=LocationModel(column="A", shelving_row=1, side="LEFT", row=0))],
    orders=[OrderModel(id="order1", items=[
        ProductModel(id="p0", volume=1232, location=LocationModel(column="AB", shelving_row=4, side="RIGHT", row=17)),
    ])],
)
```

**Named locations with a distance table.** For a building that is not such a grid, `locations`
lists the ids of its places (at most 4096) and `distances` the walk between them, `{from id: {to id: meters}}`
in whole meters (0..1000000). The `location` of a trolley or an order item is then one of the
ids, and the response's stops name them the same way.

```python
request = OrderPickingRequest(
    time_limit_seconds=2,
    locations=["dock", "aisle-1", "aisle-2"],
    distances={"dock": {"aisle-1": 12, "aisle-2": 20}, "aisle-1": {"aisle-2": 9}},
    trolleys=[TrolleyModel(id="1", bucket_count=4, bucket_capacity=48000, location="dock")],
    orders=[OrderModel(id="order1", items=[ProductModel(id="p0", volume=1232, location="aisle-2")])],
)
```

- A location is 0 meters from itself (a distance given for it must be 0).
- The table may be asymmetric (a one-way aisle): `distances["a"]["b"]` and `distances["b"]["a"]`
  are read separately when both are given.
- A pair given one way only is as long the other way, so a symmetric table needs each pair once.
- A pair of `locations` given neither way is refused: the table has to say how far every
  location is from every other one, also those no trolley or item is at. Nothing is derived by
  walking through a third location, and the distances given are used as they are.
- An id in `distances` that is not in `locations` is refused.
- The table is one number for every pair, so it grows with the square of `locations`: 1,000
  locations are a million distances at most (half of that when each pair is given once), which
  the request model reads and lays out in about 0.2 s and which have to fit the body an API
  accepts. `locations` needs only the places the request's trolleys and items are at, not the
  whole building, and holds at most 4096. A caller that already has the table as a matrix can
  skip the dict: `op.Warehouse.from_distances` takes it as it is.

The distances are in the unit the score counts in. The default weight of
`order_split_by_trolley_cost`, 1000, prices one more trolley on an order at 1000 of them.

**One kind per request.** `warehouse` together with `locations`, a grid location in a request
with `locations`, or an id in a request without them is refused with a message that says so.

On the compiled side the layout is `OrderPickingSolution.warehouse`: `op.Warehouse(column_count,
shelving_row_count, shelving_width, shelving_height, shelving_padding)` for a grid,
`op.Warehouse.from_distances(location_ids, distances)` (a square list of lists) for named
locations, whose locations come from `warehouse.location(id)`; see [`python.md`](python.md).

While a solve runs, every distance is a lookup in one table, for both kinds: on a grid the
solver fills it for the locations in use (up to 2048 distinct ones; beyond that it computes
each distance as it goes, which is what it did for every solve before).

The CP-SAT reference of the [benchmark](#benchmark) knows the default grid only, so a request
with `warehouse`, or with `locations` and `distances`, solves on the engine only until the
reference model follows.

## Constraints

The score is the engine's `Score` type as it is, with no mapping.

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost per trolley (0 = no violation) |
|---|---|---|---|---|
| `trolley_capacity_cost` | `TrolleyCapacityCost` | hard | 1 | buckets short: `max(0, sum over its orders of ceil(order volume / bucket capacity) - bucket count)` (orders never share a bucket) |
| `distance_from_previous_pick_task_cost` | `DistanceFromPreviousPickTaskCost` | soft | 1 | warehouse distance (m) start -> first pick -> ... -> last pick |
| `distance_from_last_pick_task_to_path_origin_cost` | `DistanceFromLastPickTaskToPathOriginCost` | soft | 1 | warehouse distance (m) from the last pick back to the start; 0 for an empty trolley |
| `order_split_by_trolley_cost` | `OrderSplitByTrolleyCost` | soft | 1000 | distinct orders with a pick on the trolley — summed, the (order, trolley) pairs. **Not** 0 for an unsplit order: an order on one trolley counts 1, so the minimum is the number of orders with assigned picks |

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
from optimaru.order_picking.constraints import (
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

## Starting from an existing solution

```python
from optimaru.order_picking.models import solve_request, with_initial_solution

solution = solve_request(request)                        # request: an OrderPickingRequest
next_request = with_initial_solution(request, solution)  # or solution.model_dump(mode="json")
better = solve_request(next_request)
```

`with_initial_solution` returns a new request whose trolleys carry `initial_route`, a list of pick task ids. The solve keeps those routes as its starting point and construction places only the tasks in no route, so a partial solution works. With `time_limit_seconds=0` a complete solution comes back as it went in. An unknown id or a task routed twice raises `ValueError`.

A pick task id is `<order id>-<item position>`, so keep each order's `items` in the order they were solved in: moved, the same id names a different item.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **sequence** shape; the domain solver that configures the
engine is `optimaru/order_picking/cpp/op/solver.hpp` + `optimaru/order_picking/cpp/op/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). `OrderConsolidateMove` (pulls an order's items onto one trolley) is defined in `optimaru/order_picking/cpp/op/solver.cpp` and registered there when `custom_moves.order_consolidate` is on — use it as the template. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/order_picking/cpp/op/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** Elements are placed by cheapest insertion in index order. Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one new
constraint through this use case's files in six steps: changed from Python, written as a
component, tested, made cheaper per move, moved to the default fast path and measured.

## Benchmark

[`benchmarks/order-picking`](../../../benchmarks/order-picking) compares this use case with OR-Tools CP-SAT on the same instances.
