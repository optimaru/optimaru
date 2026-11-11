# optimaru.vehicle_routing (Python)

Python bindings for the C++ vehicle-routing example in
[`optimaru/vehicle_routing/cpp`](../../../optimaru/vehicle_routing/cpp) — one solver, one module.

**The problem:** vehicles with capacities start from their depots;
customer visits have demands, time windows and service durations. Decide
which vehicle serves which visits, in what order, driving as little as
possible. Routes are open by default — a vehicle ends its day at its last
visit; set `return_to_depot=True` on a vehicle to charge and time the leg
back to its depot. `RouteSolution.vehicle_metrics` reports per-vehicle stops,
load, driving / waiting / service seconds and the route end time; these are
post-processed from the finished routes (`vehicle_metrics()`) and never
scored.

## Install and test

```sh
OPTIMARU_USE_CASES=vehicle_routing uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/vehicle_routing
```

## Usage

```python
from optimaru import vehicle_routing as vrp

locations = [vrp.Location(37.5665, 126.9780),   # index 0: vehicle home
             vrp.Location(37.5700, 126.9820)]   # index 1: a customer
vehicles = [vrp.Vehicle("V1", capacity=10, home_location_index=0,
                        departure_time=7 * 3600)]
visits = [vrp.Visit("C1", "Customer 1", location_index=1, demand=3,
                    min_start_time=8 * 3600, max_end_time=18 * 3600,
                    service_duration=600)]
plan = vrp.VehicleRoutePlan("demo", locations, vehicles, visits)

vrp.solve(plan, time_limit_seconds=2.0, random_seed=42)   # in place; returns the plan

print(plan.score)                      # 0hard/0medium/-38soft
print(plan.routes())                   # {'V1': ['C1']}
```

`vrp.solve` also takes `parallel_solver_count`, `stop_at_first_feasible`,
`full_assert` (re-verifies every incremental score against a full recompute; a
debugging aid, much slower), `on_new_best`, `constraints` and a full `config`: a
`vrp.SolverConfig`, whose fields the keywords override on a copy.
`vrp.Solver(config).solve(plan)` is the same solve one level down.
`config.acceptor_type` chooses the rule local search accepts a move by (late
acceptance by default); its parameters are listed under
[Choosing the acceptance rule](../README.md#choosing-the-acceptance-rule).

- Times are integer **seconds from a common day start** (`8 * 3600` = 08:00).
- The driving-time matrix is precomputed from the haversine distance when the
  plan is constructed.
- Score (hard, medium, soft; lexicographic), at the default weights: **hard** =
  capacity excess (`vehicle_capacity_cost`) + time-window lateness
  (`max_end_time_cost`) (0 = feasible), **medium** = −(penalty minutes of
  unassigned visits, `unassigned_visits_cost`; folded into the engine's soft as
  `medium * 10^9`), **soft** = −(total driving seconds, `travel_time_cost`).
- Visits that share `Visit.group_index` are one job: they ride one vehicle
  together or all stay unassigned, and `Visit.after_visit_indices` puts a
  partial order on them (`visit_group_cost`, `visit_order_cost`, both hard;
  `VisitModel.group` and `VisitModel.after` in the JSON layer). See
  [Visit groups](README.md#visit-groups).
- `vrp.constraint_info()` lists every constraint with its default level and
  weight (the C++ components' `kDefaultLevel` / `kDefaultWeight`);
  `optimaru.vehicle_routing.constraints` picks, re-weights and flips them for
  `Solver.solve_with_constraints` / `solve(..., constraints=...)`. Every
  constraint takes five knobs (`weight`, `level`, `exponent` and the keyword-only
  `offset` / `clip`) and may be picked several times, each pick with its own knobs
  (`[TravelTimeCost(), TravelTimeCost(exponent=2, offset=3600)]` is the driving time
  plus the square of every second beyond one hour per vehicle). The constraint table
  and the knobs are in the [use-case README](README.md#constraints).

The default VRP configuration also enables a granular descent (Toth & Vigo 2003):
nearby route pairs are searched with best-improvement relocate, swap, 2-opt,
2-opt* and bounded CROSS-exchange moves. Set `granular_descent = False` (or
lower `descent_weight`) when a cheaper sampled neighbourhood is preferred.

## JSON in, JSON out (pydantic)

```python
from optimaru.vehicle_routing import solve
from optimaru.vehicle_routing.models import (RoutePlanRequest, RoutingInput, VehicleModel,
                                VisitModel)

request = RoutePlanRequest(
    timelimit=2,
    model_input=RoutingInput(
        vehicles=[VehicleModel(id="V1", capacity=10, depot=(37.5665, 126.9780))],
        visits=[VisitModel(id="C1", location=(37.57, 126.982), demand=3,
                           min_start_time=8 * 3600, max_end_time=18 * 3600,
                           service_duration=600)],
    ),
)
solution = solve(request)              # -> RouteSolution
print(solution.routes, solution.score, solution.total_driving_seconds)
```

Everything round-trips through JSON (`model_validate_json` /
`model_dump_json`); fields are snake_case, classes PascalCase.

## Files

```
optimaru/vehicle_routing/
├── __init__.py       re-exports the core
├── constraints.py    the constraint specs (one class per C++ component), built on optimaru/_constraint.py
├── models.py         pydantic request/response layer
├── _core.pyi         stubs of the compiled module
└── cpp/              bindings.cpp (the _core module, a thin wrapper), optimaru/vehicle_routing/cpp/include/vrp/, src/
tests/vehicle_routing/   correctness + model tests (pytest); cpp/ holds the C++ tests
```
