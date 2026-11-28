# optimaru.order_picking (Python)

The Python package wrapping, with pybind11, the C++ implementation of the
**order-picking** use case (`optimaru/order_picking/cpp`, on the generic engine `core/optimaru/`).
Derived from the Timefold Quickstarts `order-picking` example (Apache-2.0; see the repository NOTICE).

## Problem

Pick order items onto trolleys in a warehouse: by default a grid of 5 shelving columns A-E x 3 rows,
left/right sides x rows 0-10; a grid of another size or named locations with a distance table when the
problem says so ([Layouts](#layouts)).

- **decision**: `Trolley.pick_task_indices` — the list of `PickTask` indices the
  trolley picks, in visiting order.
- **derived values**: `PickTask.trolley_index / previous_pick_task / next_pick_task`
  (the trolley carrying the pick and its neighbours in that trolley's list) — derived
  from the lists and kept current by `calculate_score` / `solve`.

## Install and test

```bash
OPTIMARU_USE_CASES=order_picking uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/order_picking
```

## Usage

```python
from optimaru import order_picking as op

solution = op.generate_demo_data()   # the demo warehouse (deterministic, seed 37)
op.solve(solution, time_limit_seconds=5.0, random_seed=0)
print(solution.score)             # e.g. 0hard/-8652soft
print(solution.routes())          # {trolley id: [pick task id, ...]}
```

`generate_demo_data(trolleys_count=5, bucket_count=4, bucket_capacity=48000, orders_count=8,
random_seed=37)` builds the demo warehouse. Its name before 0.5.1, `build_demo_data()`, still works (an alias
of the same function).

JSON in/out is `optimaru.order_picking.models` (pydantic v2):

```python
from optimaru.order_picking.models import OrderPickingRequest, solve_request

response = solve_request(OrderPickingRequest.model_validate(request_dict))
print(response.feasible, response.distance_by_trolley)
```

## Layouts

`OrderPickingSolution(orders, trolleys, pick_tasks, warehouse=None)` takes the layout its locations
are in; `None` is the default grid, and `solution.warehouse` reads it back.

```python
# A grid of another size: 30 columns x 4 shelving rows, shelvings 2 m x 20 m, 3 m apart.
warehouse = op.Warehouse(column_count=30, shelving_row_count=4, shelving_height=20)
start = op.WarehouseLocation(column=0, shelving_row=0, side=op.Side.LEFT, row=0)
far = op.WarehouseLocation(column=27, shelving_row=3, side=op.Side.RIGHT, row=17)   # (AB,4)
solution = op.OrderPickingSolution(
    [op.Order("order1")], [op.Trolley("1", 4, 48000, start)], [op.PickTask("order1-0", 0, 1232, far)], warehouse
)
print(op.calculate_distance(start, far, warehouse), far.shelving_id)

# Named locations: distances[i][j] is the whole meters from location_ids[i] to location_ids[j].
warehouse = op.Warehouse.from_distances(["dock", "aisle-1", "aisle-2"], [[0, 12, 20], [12, 0, 9], [20, 9, 0]])
dock, aisle = warehouse.location("dock"), warehouse.location("aisle-2")
solution = op.OrderPickingSolution(
    [op.Order("order1")], [op.Trolley("1", 4, 48000, dock)], [op.PickTask("order1-0", 0, 1232, aisle)], warehouse
)
op.solve(solution, time_limit_seconds=1.0)
print(op.distance_to_travel(solution, 0))   # 40: there and back
```

- `Warehouse(column_count=5, shelving_row_count=3, shelving_width=2, shelving_height=10, shelving_padding=3)`:
  a grid, in whole meters; `ValueError` outside 1..18278 columns, 1..10000 shelving rows and 1..1000 for the
  three sizes. `WarehouseLocation(column, shelving_row, side, row)` is 0-based and checks only that
  nothing is negative: whether a location is inside the grid is checked where the warehouse is known,
  by `OrderPickingSolution(...)` and again by `solve`, `calculate_score`, `distance_to_travel` and
  `constraint_costs` (`ValueError`, naming the trolley or pick task), and by `calculate_distance` for
  its two locations.
- `Warehouse.from_distances(location_ids, distances)`: named locations. The table is square over the
  ids (at most 4096, `Warehouse.MAX_LOCATIONS`) and may be asymmetric; `ValueError` for a repeated id, a distance outside 0..1000000 or a
  location that is not 0 from itself. `warehouse.location(id)` is the location to give a trolley or a
  pick task (`location.index` is its position in `warehouse.location_ids`, -1 for a grid location),
  and `warehouse.location_id(location)` names it again. A named location belongs to the warehouse
  that made it: a solution of another warehouse refuses it (`ValueError`).
- One solution, one kind: a grid location in a warehouse of named locations, or the reverse, is a
  `ValueError`.
- `calculate_distance(start, end, warehouse=None)` measures in the default grid without a warehouse.

In `optimaru.order_picking.models` the same two kinds are the request's `warehouse`
(`WarehouseModel`), or its `locations` and `distances` (a dict of dicts, where a pair given one way
only is as long the other way); a `location` is a `LocationModel` (column letters `A`..`Z`, `AA`, ...)
or the id of a named location. The fields, their ranges and what is refused are in the
[guide](README.md#warehouse-layout).

`SolverConfig` adjusts the engine knobs (time budget, sequence move weights, ALNS, nearby, ScoreChecks::Full,
`parallel_solver_count` multistart, ...) — see `_core.pyi`.

With an API key set (`optimaru.use_api`, `OPTIMARU_API_KEY`), `solve_request` runs on the
optimaru API and `solve(solution)` is refused: a compiled solution is solved in place, which the API
cannot do. See [Solving on the optimaru API](../README.md#solving-on-the-optimaru-api).

## Constraints

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost per trolley (0 = no violation) |
| --- | --- | --- | --- | --- |
| `trolley_capacity_cost` | `TrolleyCapacityCost` | hard | 1 | buckets short: the sum of required buckets per order (`ceil(order volume / bucket capacity)`, orders never share a bucket) beyond the trolley's buckets |
| `distance_from_previous_pick_task_cost` | `DistanceFromPreviousPickTaskCost` | soft | 1 | warehouse distance from the previous pick (the trolley's start for the first pick) |
| `distance_from_last_pick_task_to_path_origin_cost` | `DistanceFromLastPickTaskToPathOriginCost` | soft | 1 | distance from the last pick back to the start |
| `order_split_by_trolley_cost` | `OrderSplitByTrolleyCost` | soft | 1000 | distinct orders on the trolley (summed: the trolleys an order spans); an unsplit order still counts 1 |

Score: the engine's `Score(int64 hard, double soft)`, at the default weights
hard = -bucket shortfall, soft = -(travel distance + 1000 x (order, trolley) pairs).
The domain has no medium level, so `level="medium"` raises `ValueError` (`level="hard"` / `"soft"`
flip any constraint).
`order_picking.constraint_info()` lists the table's defaults (read off the C++ components),
`order_picking.constraint_costs(solution)` the raw per-trolley costs — the only per-constraint
surface.

Pick, reweight or flip constraints with `optimaru.order_picking.constraints`:

```python
from optimaru import order_picking as op
from optimaru.order_picking.constraints import (
    DistanceFromPreviousPickTaskCost, OrderSplitByTrolleyCost, TrolleyCapacityCost, score,
)

op.solve(solution, constraints=[TrolleyCapacityCost(), OrderSplitByTrolleyCost(weight=100)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping per trolley (clip=True: only the meters beyond 100 count).
raw = op.constraint_costs(solution)
score(raw, [DistanceFromPreviousPickTaskCost(), DistanceFromPreviousPickTaskCost(weight=0.1, exponent=2, offset=100)])
```

## Parameters and their costs

A request is refused when it sets a field that acts only through a cost while its
constraints leave that cost out ([Parameters and their costs](../README.md#parameters-and-their-costs)).
This use case has no such field: every field a cost reads is required, so
`models.parameter_uses(model)` is always empty. The layout (`warehouse`, or `locations` and
`distances`) has a default but is no such field either: the response reports the meters walked
from it whatever the constraints.
