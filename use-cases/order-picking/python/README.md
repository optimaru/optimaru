# order_picking (Python)

The Python package wrapping, with pybind11, the C++ port of the example model
**order-picking** (`../cpp`, on the generic engine `include/optimaru/`).

## Problem

Pick order items onto trolleys in a warehouse (5 shelf columns A-E x 3 rows, left/right sides x rows 0-10).

- **planning entity**: `Trolley` — `pick_task_indices` is the `@PlanningListVariable`
  (the list of `PickTask` indices the trolley picks, in visiting order).
- **shadow variables**: `PickTask.trolley_index / previous_pick_task / next_pick_task`
  (Java's `@InverseRelationShadowVariable` / `@PreviousElementShadowVariable` /
  `@NextElementShadowVariable`) — derived from the lists and kept current by
  `calculate_score` / `solve`.

## Constraints (full port of the Java `OrderPickingConstraintProvider`)

| Level | Constraint | What it measures |
| --- | --- | --- |
| hard | requiredNumberOfBuckets | when the sum of required buckets per order (`ceil(order volume / bucket capacity)`, orders never share a bucket) exceeds the trolley's buckets, the excess |
| soft | minimizeDistanceFromPreviousPickTask | warehouse distance from the previous pick (the trolley's start for the first pick) |
| soft | minimizeDistanceFromLastPickTaskToPathOrigin | distance from the last pick back to the start |
| soft | minimizeOrderSplitByTrolley | trolleys an order spans (countDistinct) x 1000 |

Score mapping: Java `HardSoftScore`(int) → engine `HardSoftScore(int64 hard, double soft)`
1:1 (hard = -bucket shortfall, soft = -(travel distance + 1000 x order splits)). No level folding.

## Install / test

```bash
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

```python
import order_picking as op

solution = op.build_demo_data()   # Java DemoDataGenerator dataset (bit-identical Random(37) replay)
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

`SolverConfig` adjusts the engine knobs (time budget, LIST move weights, ALNS, nearby, FULL_ASSERT,
`parallel_solver_count` multistart, ...) — see `_core.pyi`.
