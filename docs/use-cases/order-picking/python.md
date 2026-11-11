# optimaru.order_picking (Python)

The Python package wrapping, with pybind11, the C++ implementation of the
**order-picking** use case (`optimaru/order_picking/cpp`, on the generic engine `optimaru/cpp/optimaru/`).
Derived from the Timefold Quickstarts `order-picking` example (Apache-2.0; see the repository NOTICE).

## Problem

Pick order items onto trolleys in a warehouse (5 shelf columns A-E x 3 rows, left/right sides x rows 0-10).

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

solution = op.build_demo_data()   # the demo warehouse (deterministic, seed 37)
op.solve(solution, time_limit_seconds=5.0, random_seed=0)
print(solution.score)             # e.g. 0hard/-8652soft
print(solution.routes())          # {trolley id: [pick task id, ...]}
```

JSON in/out is `optimaru.order_picking.models` (pydantic v2). `solve` takes the
problem model and returns a solved copy of it:

```python
from optimaru.order_picking.models import OrderPickingInput

solved = op.solve(OrderPickingInput.model_validate(problem_dict), time_limit_seconds=5.0)
print(solved.score_hard, solved.total_distance, [t.distance for t in solved.trolleys])
```

`solve` fills the result fields of the copy: `route` (the pick tasks in order) and
`distance` on each trolley, and `unassigned`, `score_hard`, `score_soft`,
`total_distance` and `order_split_count` on the problem. They are `None` before a
solve and are never read as input. The earlier pydantic shape still works: an
`OrderPickingRequest` (the same trolleys and orders with `timelimit`, `random_seed`,
`num_workers`) passed to `solve` or to `models.solve_request` returns an
`OrderPickingResponse`.

`SolverConfig` adjusts the engine knobs (time budget, sequence move weights, ALNS, nearby, ScoreChecks::Full,
`parallel_solver_count` multistart, ...) — see `_core.pyi`.

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
