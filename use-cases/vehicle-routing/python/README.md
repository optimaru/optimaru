# vrp_routing — Vehicle Routing for Python

Python bindings for the C++ vehicle-routing example in
[`../cpp`](../cpp) — one solver, one module.

**The problem:** vehicles with capacities start from their depots;
customer visits have demands, time windows and service durations. Decide
which vehicle serves which visits, in what order, driving as little as
possible. Routes are open by default — a vehicle ends its day at its last
visit; set `return_to_depot=True` on a vehicle to charge and time the leg
back to its depot. `RouteSolution.vehicle_metrics` reports per-vehicle stops,
load, driving / waiting / service seconds and the route end time; these are
post-processed from the finished routes (`vehicle_metrics()`) and never
scored.

## Build & run

```sh
cd use-cases/vehicle-routing/python
python3 setup.py build_ext --inplace   # compiles vrp_routing._core
python3 -m pytest tests/
```

```python
import vrp_routing as vrp

locations = [vrp.Location(37.5665, 126.9780),   # index 0: vehicle home
             vrp.Location(37.5700, 126.9820)]   # index 1: a customer
vehicles = [vrp.Vehicle("V1", capacity=10, home_location_index=0,
                        departure_time=7 * 3600)]
visits = [vrp.Visit("C1", "Customer 1", location_index=1, demand=3,
                    min_start_time=8 * 3600, max_end_time=18 * 3600,
                    service_duration=600)]
plan = vrp.VehicleRoutePlan("demo", locations, vehicles, visits)

cfg = vrp.SolverConfig()
cfg.spent_limit_seconds = 2.0
cfg.random_seed = 42
vrp.Solver(cfg).solve(plan)            # in place

print(plan.score)                      # e.g. 0hard/0medium/-1234soft
print(plan.routes())                   # {"V1": ["C1"]}
```

- Times are integer **seconds from a common day start** (`8 * 3600` = 08:00).
- The driving-time matrix is precomputed from the haversine distance when the
  plan is constructed.
- Score (`HardMediumSoft`, lexicographic): **hard** = capacity excess +
  time-window lateness (0 = feasible), **medium** = −(service minutes of
  unassigned visits), **soft** = −(total driving seconds).

The default VRP configuration also enables a PyVRP-style granular descent:
nearby route pairs are searched with best-improvement relocate, swap, 2-opt,
2-opt* and bounded CROSS-exchange moves. Set `granular_descent = False` (or
lower `descent_weight`) when a cheaper sampled neighbourhood is preferred.

## JSON in, JSON out (pydantic, optional)

```python
from vrp_routing.models import (RoutePlanRequest, RoutingInput, VehicleModel,
                                VisitModel, solve_request)

request = RoutePlanRequest(
    timelimit=2,
    model_input=RoutingInput(
        vehicles=[VehicleModel(id="V1", capacity=10, depot=(37.5665, 126.9780))],
        visits=[VisitModel(id="C1", location=(37.57, 126.982), demand=3,
                           min_start_time=8 * 3600, max_end_time=18 * 3600,
                           service_duration=600)],
    ),
)
solution = solve_request(request)      # -> RouteSolution
print(solution.routes, solution.score, solution.total_driving_seconds)
```

Everything round-trips through JSON (`model_validate_json` /
`model_dump_json`); fields are snake_case, classes PascalCase.

## Files

```
bindings.cpp             pybind11 module (PYBIND11_MODULE(_core)) — thin wrapper
vrp_routing/__init__.py  re-exports the core
vrp_routing/models.py    pydantic request/response layer (optional)
setup.py, pyproject.toml build (links ../cpp)
tests/                   correctness + model tests (pytest)
```
