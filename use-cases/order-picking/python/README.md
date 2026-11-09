# order_picking (Python)

The Python package wrapping, with pybind11, the C++ port of the example model
**order-picking** (`../cpp`, on the generic engine `include/optimaru/`).

## Problem

Pick order items onto trolleys in a warehouse (5 shelf columns A-E x 3 rows, left/right sides x rows 0-10).

- **planning entity**: `Trolley` — `pick_task_indices` is the `@PlanningListVariable`
  (the list of `PickTask` indices the trolley picks, in visiting order).
- **derived values**: `PickTask.trolley_index / previous_pick_task / next_pick_task`
  (Java's `@InverseRelationShadowVariable` / `@PreviousElementShadowVariable` /
  `@NextElementShadowVariable`) — derived from the lists and kept current by
  `calculate_score` / `solve`.

## Constraints (full port of the Java `OrderPickingConstraintProvider`)

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost per trolley (0 = no violation) |
| --- | --- | --- | --- | --- |
| `trolley_capacity_cost` | `TrolleyCapacityCost` | hard | 1 | buckets short: the sum of required buckets per order (`ceil(order volume / bucket capacity)`, orders never share a bucket) beyond the trolley's buckets |
| `distance_from_previous_pick_task_cost` | `DistanceFromPreviousPickTaskCost` | soft | 1 | warehouse distance from the previous pick (the trolley's start for the first pick) |
| `distance_from_last_pick_task_to_path_origin_cost` | `DistanceFromLastPickTaskToPathOriginCost` | soft | 1 | distance from the last pick back to the start |
| `order_split_by_trolley_cost` | `OrderSplitByTrolleyCost` | soft | 1000 | distinct orders on the trolley (summed: the trolleys an order spans, countDistinct); an unsplit order still counts 1 |

Score mapping: Java `Score`(int) → engine `Score(int64 hard, double soft)`
1:1 (hard = -bucket shortfall, soft = -(travel distance + 1000 x (order, trolley) pairs)). No level folding:
the domain has no medium level, so `level="medium"` raises `ValueError` (`level="hard"` / `"soft"`
flip any constraint).
`order_picking.constraint_info()` lists the table's defaults (read off the C++ components),
`order_picking.constraint_costs(solution)` the raw per-trolley costs — the only per-constraint
surface.

Pick, reweight or flip constraints with `order_picking.constraints`:

```python
import order_picking as op
from order_picking.constraints import (
    DistanceFromPreviousPickTaskCost, OrderSplitByTrolleyCost, TrolleyCapacityCost, score,
)

op.solve(solution, constraints=[TrolleyCapacityCost(), OrderSplitByTrolleyCost(weight=100)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping per trolley (clip=True: only the meters beyond 100 count).
raw = op.constraint_costs(solution)
score(raw, [DistanceFromPreviousPickTaskCost(), DistanceFromPreviousPickTaskCost(weight=0.1, exponent=2, offset=100)])
```

## Install / test

```bash
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

```python
import order_picking as op

solution = op.build_demo_data()   # the demo warehouse (deterministic, seed 37)
op.solve(solution, time_limit_seconds=5.0, random_seed=0)
print(solution.score)             # e.g. 0hard/-8652soft
print(solution.routes())          # {trolley id: [pick task id, ...]}
```

JSON in/out is `order_picking.models` (pydantic v2, optional dependency):

```python
from order_picking.models import OrderPickingRequest, solve_request

response = solve_request(OrderPickingRequest.model_validate(request_dict))
print(response.feasible, response.distance_by_trolley)
```

`SolverConfig` adjusts the engine knobs (time budget, sequence move weights, ALNS, nearby, ScoreChecks::Full,
`parallel_solver_count` multistart, ...) — see `_core.pyi`.
