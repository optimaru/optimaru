# optimaru.vehicle_routing (Python)

Python bindings for the C++ vehicle-routing example in
[`optimaru/vehicle_routing/cpp`](../../../optimaru/vehicle_routing/cpp) — one solver, one module.

**The problem:** vehicles with capacities start from their depots;
customer visits have demands, time windows and service durations. Decide
which vehicle serves which visits, in what order, driving as little as
possible. Routes are open by default — a vehicle ends its day at its last
visit; give a vehicle an `end_depot` (its start depot or another) to charge
and time the leg there. `RouteSolution.vehicle_metrics` reports per-vehicle stops,
load, driving seconds and meters, waiting and service seconds and the route end
time; these are post-processed from the finished routes (`vehicle_metrics()`)
and never scored.

## Install and test

```sh
OPTIMARU_USE_CASES=vehicle_routing uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/vehicle_routing
```

## Usage

```python
from optimaru import vehicle_routing as vrp

locations = [vrp.Location(37.5665, 126.9780),   # index 0: the depot
             vrp.Location(37.5700, 126.9820)]   # index 1: a customer
vehicles = [vrp.Vehicle("V1", capacity=10, start_location_index=0,
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
- Vehicles of one depot should share its location index (`start_location_index`):
  the operators that skip equivalent idle vehicles compare indices.
- `VehicleRoutePlan` takes one driving-seconds and one driving-meters matrix per
  travel profile (`durations=`, `distances=`, lists of square matrices over the
  locations; `Vehicle.profile_index` picks one). A matrix not given is
  precomputed from the coordinates when the plan is constructed: haversine
  meters, and those meters at 50 km/h for the seconds. `Vehicle.end_location_index`
  is where the route ends, -1 (the default) for an open route.
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
- `VehicleRoutePlan(..., profile_visits=[vrp.ProfileVisits(forbidden, preferred)])`
  lists, per kind of vehicle, the visits it may not serve and those it is preferred
  for; `Vehicle.profile_visits_index` picks one (-1 for none). Scored by
  `profile_forbidden_visit_cost` (hard, always on) and `profile_preferred_visit_cost`
  (soft); `TravelProfile.forbidden_visits` and `preferred_visits` in the JSON layer.
  See [Profile visits](README.md#profile-visits).
- `vrp.constraint_info()` lists every constraint with its default level and
  weight (the C++ components' `kDefaultLevel` / `kDefaultWeight`);
  `optimaru.vehicle_routing.constraints` picks, re-weights and flips them for
  `Solver.solve_with_constraints` / `solve_request(..., constraints=...)`. Every
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

With an API key set (`optimaru.use_api`, `OPTIMARU_API_KEY`), `solve_request` runs on the
optimaru API and `solve(plan)` is refused: a compiled plan is solved in place, which the API
cannot do. See [Solving on the optimaru API](../README.md#solving-on-the-optimaru-api).

```python
from optimaru.vehicle_routing.models import (DepotModel, RoutePlanRequest, RoutingInput,
                                VehicleModel, VisitModel, solve_request)

request = RoutePlanRequest(
    time_limit_seconds=2,
    model_input=RoutingInput(
        depots=[DepotModel(id="D1", location=(37.5665, 126.9780))],
        vehicles=[VehicleModel(id="V1", capacity=10, start_depot="D1")],
        visits=[VisitModel(id="C1", location=(37.57, 126.982), demand=3,
                           min_start_time=8 * 3600, max_end_time=18 * 3600,
                           service_duration=600)],
    ),
)
solution = solve_request(request)      # -> RouteSolution
print(solution.routes, solution.score, solution.total_driving_seconds)
```

### Depots and travel profiles

A route starts at its vehicle's `start_depot` and ends at its `end_depot`; without
one (the default) the route is open and ends at its last visit. Depots and visits
share one id space: no depot may have a visit's id. An idle vehicle drives nothing,
even when its two depots differ.

Driving comes from the vehicle's travel profile (`VehicleModel.profile`, `"default"`
unless set). `RoutingInput.profiles` names each profile's pairwise driving seconds
(`durations`) and meters (`distances`), keyed by depot and visit ids,
`{from_id: {to_id: value}}`:

```python
from optimaru.vehicle_routing.models import TravelProfile

profiles = {
    "car": TravelProfile(durations={"D1": {"C1": 300}, "C1": {"D1": 320}},
                         distances={"D1": {"C1": 2_100}, "C1": {"D1": 2_300}}),
    "bike": TravelProfile(durations={"D1": {"C1": 700}, "C1": {"D1": 720}}),
}
# RoutingInput(..., profiles=profiles), and VehicleModel(..., profile="bike")
```

- A pair the dicts leave out costs the profile's `missing_value`, 10,000,000 by
  default (`MISSING_TRAVEL_VALUE`: about 116 days or 10,000 km), so a route avoids
  that leg wherever it can; a place to itself costs 0 unless given.
- A profile without `durations` or without `distances`, and every vehicle when
  `profiles` is empty, drives straight lines between the coordinates: haversine
  meters, and those meters at 50 km/h for the seconds. An empty dict is refused:
  it would make every pair missing.
- A vehicle naming a profile that `profiles` does not hold is refused, as is a
  dict keyed by an id that is no depot or visit.
- `same_travel_as="car"` drives a profile on the travel of the profile `car`, which
  gives its own: profiles that differ only in their visit lists (`forbidden_visits`,
  `preferred_visits`, see [Profile visits](README.md#profile-visits)) share one set
  of dicts and one matrix in the engine.
- `solve_request` turns each distinct travel into one square matrix over the depots,
  then the visits, before the solve. For 1,000 places with both dicts given (40 MB of
  JSON), parsing and validating the request took 0.11 s and building the matrices
  0.06 s on an M4 Max.
- `travel_time_cost` prices the seconds; `travel_distance_cost` (opt-in, weight 0)
  prices the meters. Its move previews walk the candidate route, where
  `travel_time_cost` answers from prefix sums, so it slows the search on long
  routes. `RouteSolution.total_driving_meters` and each vehicle's `driving_meters`
  report the meters either way.

Everything round-trips through JSON (`model_validate_json` /
`model_dump_json`); fields are snake_case, classes PascalCase.

## Files

```
optimaru/vehicle_routing/
├── __init__.py       re-exports the core
├── constraints.py    the constraint specs (one class per C++ component), built on optimaru/_constraint.py
├── models.py         pydantic request/response layer
├── _core.pyi         stubs of the compiled module
└── cpp/              bindings.cpp (the _core module, a thin wrapper), vrp/ (the C++ core), main.cpp (the demo)
tests/vehicle_routing/   correctness + model tests (pytest); cpp/ holds the C++ tests
```
