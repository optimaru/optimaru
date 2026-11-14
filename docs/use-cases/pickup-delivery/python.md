# optimaru.pickup_delivery (Python)

Python bindings for the C++ pickup-and-delivery solver in
[`optimaru/pickup_delivery/cpp`](../../../optimaru/pickup_delivery/cpp) — one solver, one module.

**The problem:** a transport request is a pickup and a dropoff that one vehicle
must serve, pickup first, without ever exceeding its capacity in between. Each
stop has a time window and a service duration. Decide which vehicle serves which
requests, in what order, driving as little as possible. Routes are open by
default — a vehicle ends its day at its last stop; set `return_to_depot=True` on
a vehicle to charge and time the leg back home. The constraints, the request-level
operators and the scoring are explained in the [use-case README](README.md).

## Install and test

```sh
OPTIMARU_USE_CASES=pickup_delivery uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/pickup_delivery
```

```python
from optimaru import pickup_delivery as pdp

locations = [pdp.Location(37.5665, 126.9780),   # index 0: vehicle home
             pdp.Location(37.5700, 126.9820),   # index 1: the pickup
             pdp.Location(37.5740, 126.9750)]   # index 2: the dropoff
vehicles = [pdp.Vehicle("V1", capacity=10, home_location_index=0, departure_time=7 * 3600)]
nodes = [pdp.Node("R1-pickup", 1, 8 * 3600, 18 * 3600, 300, delivery_index=0, pickup=True),
         pdp.Node("R1-dropoff", 2, 8 * 3600, 18 * 3600, 300, delivery_index=0, pickup=False)]
deliveries = [pdp.Delivery("R1", pickup_node_index=0, dropoff_node_index=1, demand=3)]
plan = pdp.PdpRoutePlan("demo", locations, vehicles, nodes, deliveries)

pdp.solve(plan, time_limit_seconds=2.0, random_seed=42)   # in place; returns the plan

print(plan.score)                      # 0hard/0medium/-93soft
print(plan.routes())                   # {'V1': ['R1-pickup', 'R1-dropoff']}
print(plan.unassigned_delivery_ids())  # []
```

- Times are integer **seconds from a common day start** (`8 * 3600` = 08:00).
- A `Node` is one stop, the pickup half or the dropoff half of a request
  (`Node(id, location_index, min_start_time, max_end_time, service_duration,
  delivery_index, pickup)`); a `Delivery` is the request and names its two nodes
  by index.
- Constructing a `PdpRoutePlan` accepts an explicit `driving_seconds` matrix or
  precomputes the haversine fallback.
- Each vehicle's ordered `node_indices` (its route) is the decision. A stop in no
  route is unassigned and lands in `plan.unassigned_node_indices`.
- Score (hard, medium, soft; lexicographic), at the default weights: **hard** =
  capacity peak along the route (`vehicle_capacity_cost`) + time-window lateness
  (`max_end_time_cost`) + the coupling constraints (`paired_request_cost`,
  `same_vehicle_cost`, `pickup_before_dropoff_cost`) (0 = feasible), **medium** =
  −(service minutes of the requests left unrouted, `unassigned_requests_cost`;
  folded into the engine's soft as `medium * 10^9`), **soft** = −(total driving
  seconds, `travel_time_cost`).
- `pdp.build_random_instance(request_count, vehicle_count, seed=0)` returns a
  randomly generated instance, for demos and benchmarks.

## Entry points

- `pdp.solve(plan, *, time_limit_seconds=None, random_seed=None,
  parallel_solver_count=None, stop_at_first_feasible=False, full_assert=False,
  on_new_best=None, config=None, constraints=None)` solves `plan` in place and
  returns it; the score is `plan.score`. `config` is a `pdp.SolverConfig` (None =
  the engine defaults, a 30 s budget) and the keywords override a copy of it.
  `stop_at_first_feasible` stops as soon as hard 0 is reached, `full_assert`
  re-verifies every incremental score against a full recompute (a debugging aid,
  much slower), and `on_new_best(step, score)` is called on every new best.
  `constraints` is a list of picks from `optimaru.pickup_delivery.constraints`;
  only the listed constraints are scored, and None uses the default constraints
  at their default weights.
- `pdp.Solver(config).solve(plan)` and
  `pdp.Solver(config).solve_with_constraints(plan, constraints)` are the same
  solves one level down. `SolverConfig` fields include `spent_limit_seconds`,
  `step_count_limit`, `random_seed`, `parallel_solver_count`, `full_assert`,
  `best_score_limit`, `on_new_best` and the switches of the request-level
  operators: `request_ruin_recreate` (with `request_ruin_max_requests`),
  `bundle_moves` (with `bundle_max_requests`), `pair_moves`,
  `pair_exchange_moves` and `pair_ruin`; the stubs in `_core.pyi` list them all.
  `acceptor_type` chooses the rule local search accepts a move by (late
  acceptance by default); its parameters are listed under
  [Choosing the acceptance rule](../README.md#choosing-the-acceptance-rule).
- `pdp.calculate_score(plan)` recomputes the derived fields and the score of the
  routes as they are, and stores it in `plan.score`. It is the score
  `pdp.solve(plan)` stores: the seven core constraints, or the default constraints when
  two requests or more share a group.
- `pdp.constraint_info()`, `pdp.list_constraints()` and
  `pdp.constraint_costs(plan, constraints=None)` report the catalog, the names
  and the raw per-unit costs; `optimaru.pickup_delivery.constraints.score(costs,
  constraints=None)` re-prices those raw costs in pure Python.
- `pdp.solve_request(request, constraints=None)` and
  `pdp.with_initial_solution(problem, solution)` are the JSON layer (below).

## Constraints

`pdp.constraint_info()` reports this table (the C++ components' `kDefaultLevel` /
`kDefaultWeight`), in the `pdp.list_constraints()` order:

| Constraint (Python name) | C++ component | Default level | Default weight | Scored by default |
| --- | --- | --- | --- | --- |
| `vehicle_capacity_cost` | `VehicleCapacityCost` | hard | 1 | yes |
| `max_end_time_cost` | `MaxEndTimeCost` | hard | 1 | yes |
| `travel_time_cost` | `TravelTimeCost` | soft | 1 | yes |
| `max_travel_time_cost` | `MaxTravelTimeCost` | hard | 1 | yes |
| `max_route_duration_cost` | `MaxRouteDurationCost` | hard | 1 | yes |
| `vehicle_max_end_time_cost` | `VehicleMaxEndTimeCost` | hard | 1 | yes |
| `max_stops_cost` | `MaxStopsCost` | hard | 1 | yes |
| `max_route_radius_cost` | `MaxRouteRadiusCost` | hard | 1 | yes |
| `enclosing_circle_radius_cost` | `EnclosingCircleRadiusCost` | soft | 0 | no (opt-in) |
| `min_capacity_utilization_cost` | `MinCapacityUtilizationCost` | soft | 1 | yes |
| `route_duration_cost` | `RouteDurationCost` | soft | 0 | no (opt-in) |
| `latency_cost` | `LatencyCost` | soft | 0 | no (opt-in) |
| `squared_order_age_cost` | `SquaredOrderAgeCost` | soft | 0 | no (opt-in) |
| `vehicle_fixed_cost` | `VehicleFixedCost` | soft | 1 | yes |
| `pinned_stops_cost` | `PinnedStopsCost` | hard | 1e12 | yes |
| `required_vehicle_cost` | `RequiredVehicleCost` | hard | 1000 | yes |
| `paired_request_cost` | `PairedRequestCost` | hard | 1000 | yes |
| `same_vehicle_cost` | `SameVehicleCost` | hard | 1000 | yes |
| `pickup_before_dropoff_cost` | `PickupBeforeDropoffCost` | hard | 1000 | yes |
| `request_group_cost` | `RequestGroupCost` | hard | 1000 | yes |
| `unassigned_requests_cost` | `UnassignedRequestsCost` | medium | 1 | yes |

Every constraint is flippable onto another level and none is a reward. What each
raw cost measures, the medium level and the knobs (`weight`, `level`, `exponent`
and the keyword-only `offset` / `clip`) are in the
[use-case README](README.md#constraints). `optimaru.pickup_delivery.constraints`
holds one spec class per constraint (`TravelTimeCost`), a module-level default
instance under the snake-case name (`travel_time_cost`) and `get_constraints()`,
the default list:

```python
from optimaru.pickup_delivery.constraints import TravelTimeCost, get_constraints

pdp.solve(plan, time_limit_seconds=2.0, random_seed=42, constraints=[
    *get_constraints(),                                        # the defaults
    TravelTimeCost(weight=0.001, exponent=2, offset=7200),     # + 0.001 x (seconds beyond 2 h)^2
])
```

## JSON in, JSON out (pydantic)

With an API key set (`optimaru.use_api`, `OPTIMARU_API_KEY`), `solve_request` runs on the
optimaru API and `solve(plan)` is refused: a compiled plan is solved in place, which the API
cannot do. See [Solving on the optimaru API](../README.md#solving-on-the-optimaru-api).

```python
from optimaru.pickup_delivery.models import (DeliveryModel, PdpInput, PdpRequest, StopModel,
                                             VehicleModel, solve_request)

request = PdpRequest(
    time_limit_seconds=2,
    model_input=PdpInput(
        vehicles=[VehicleModel(id="V1", capacity=10, home=(37.5665, 126.9780))],
        deliveries=[DeliveryModel(
            id="R1", demand=3,
            pickup=StopModel(id="R1-pickup", location=(37.57, 126.982), service_duration=300),
            dropoff=StopModel(id="R1-dropoff", location=(37.574, 126.975), service_duration=300),
        )],
    ),
)
solution = solve_request(request)      # -> PdpSolution
print([stop.stop_id for stop in solution.routes["V1"]])   # ['R1-pickup', 'R1-dropoff']
print(solution.score, solution.feasible, solution.total_driving_seconds)   # 0hard/0medium/-93soft True 93
```

- `PdpRequest` carries `time_limit_seconds` (seconds, default 30; alias
  `timelimit`), `random_seed`, `parallel_solver_count` (alias `num_workers`),
  `stop_at_first_feasible`, `full_assert` and `solver_parameters`, a dict of
  further `SolverConfig` fields by name.
- `PdpSolution` has `routes` (vehicle id to a list of `PdpStop` with `stop_id`,
  `delivery_id`, `pickup`, `arrival_time`), `unassigned`, `score`, `feasible`,
  `total_driving_seconds` and `vehicle_metrics`.
- `VehicleModel.max_stops` defaults to 20 here (a request counts as two; `0` lifts
  it), while the compiled `Vehicle` keeps 0, no cap.
- `with_initial_solution(request, solution)` returns a new request whose solve
  starts from `solution`; see
  [Starting from an existing solution](README.md#starting-from-an-existing-solution).
- Everything round-trips through JSON (`model_validate_json` / `model_dump_json`).

## Files

```
optimaru/pickup_delivery/
├── __init__.py       re-exports the core, solve, solve_request, with_initial_solution
├── constraints.py    the constraint specs (one class per C++ component), built on optimaru/_constraint.py
├── models.py         pydantic request/response layer
├── _core.pyi         stubs of the compiled module
└── cpp/              bindings.cpp (the _core module), include/pdp/, src/
tests/pickup_delivery/   pytest suite; cpp/ holds the C++ tests (pdp_test, pdp_plugin_test)
```
