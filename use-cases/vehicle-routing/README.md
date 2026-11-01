# Vehicle Routing

Domain model, constraints and API details: [`python/`](python/README.md) and [`cpp/`](cpp/README.md).

## Usage

```sh
cd use-cases/vehicle-routing/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import vrp_routing as vrp

locations = [vrp.Location(37.5665, 126.9780),   # index 0: vehicle home
             vrp.Location(37.5700, 126.9820)]   # index 1: a customer
vehicles = [vrp.Vehicle("V1", capacity=10, home_location_index=0, departure_time=7 * 3600)]
visits = [vrp.Visit("C1", "Customer 1", location_index=1, demand=3,
                    min_start_time=8 * 3600, max_end_time=18 * 3600, service_duration=600)]
plan = vrp.VehicleRoutePlan("demo", locations, vehicles, visits)

config = vrp.SolverConfig()
config.spent_limit_seconds = 5.0
config.random_seed = 42
vrp.Solver(config).solve(plan)                  # solves in place
print(plan.score, plan.routes())
```

`Solver(config).solve(plan)` works on the engine objects; for JSON in/out use `vrp_routing.models.solve_request(...)`, which also takes `constraints=` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/vehicle-routing/cpp -B build/vehicle-routing && cmake --build build/vehicle-routing -j
ctest --test-dir build/vehicle-routing   # vrp_test, vrp_plugin_test (assembly == monolith,
                                         # raw costs by hand, catalog, medium level)
```

## Starting from an existing solution

```python
from vrp_routing.models import solve_request, with_initial_solution

solution = solve_request(request)                        # request: a RoutePlanRequest
next_request = with_initial_solution(request, solution)  # or solution.model_dump(mode="json")
better = solve_request(next_request)
```

`with_initial_solution` returns a new request whose vehicles carry `initial_route`, a list of visit ids. The solve keeps those routes as its starting point and construction places only the visits in no route, so a partial solution works and the `unassigned` visits get another try. With `timelimit=0` a complete solution comes back as it went in. An unknown id or a visit routed twice raises `ValueError`.

## Constraints

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
| --- | --- | --- | --- | --- |
| `vehicle_capacity_cost` | `VehicleCapacityCost` | hard | 1 | per vehicle: summed visit demand above the capacity |
| `max_end_time_cost` | `MaxEndTimeCost` | hard | 1 | per vehicle: time-window lateness (arrival + service past the visit's `max_end_time`), in whole minutes rounded up |
| `travel_time_cost` | `TravelTimeCost` | soft | 1 | per vehicle: driving seconds — home → visits, plus the leg back home only when the vehicle's `return_to_depot` is set |
| `max_travel_time_cost` | `MaxTravelTimeCost` | hard | 1 | per vehicle: driving seconds above `max_travel_time` (0 = no limit) |
| `max_route_duration_cost` | `MaxRouteDurationCost` | hard | 1 | per vehicle: route duration seconds above `max_route_duration` (0 = no limit) |
| `vehicle_max_end_time_cost` | `VehicleMaxEndTimeCost` | hard | 1 | per vehicle: seconds the route ends after the vehicle's `max_end_time` (0 = no limit) |
| `max_stops_cost` | `MaxStopsCost` | hard | 1 | per vehicle: visits above `max_stops` (0 = no cap) |
| `max_route_radius_cost` | `MaxRouteRadiusCost` | hard | 1 | per vehicle: meters the farthest visit lies beyond `max_route_radius_meters` from home (0 = no limit) |
| `enclosing_circle_radius_cost` | `EnclosingCircleRadiusCost` | soft | 0 (opt-in) | per vehicle: radius in meters of the smallest circle around its visits (0 with fewer than two) |
| `min_capacity_utilization_cost` | `MinCapacityUtilizationCost` | soft | 1 | per used vehicle: demand short of `capacity * min_load_ratio` (0 = no target) |
| `route_duration_cost` | `RouteDurationCost` | soft | 0 (opt-in) | per used vehicle: seconds from departure to the route end |
| `latency_cost` | `LatencyCost` | soft | 0 (opt-in) | per vehicle: seconds from departure to each visit's arrival, summed |
| `vehicle_fixed_cost` | `VehicleFixedCost` | soft | 1 | per used vehicle: its `fixed_cost` |
| `unassigned_visits_cost` | `UnassignedVisitsCost` | medium | 1 | penalty minutes of the visits left unrouted (`unassigned_penalty_minutes`, by default the service time in whole minutes) |

That is the `list_constraints()` order: the thirteen per-vehicle unit constraints
(each vehicle; `constraint_costs()` lists the unassigned pool as one more cell, always
0, which no route constraint evaluates), then the global `unassigned_visits_cost`. `vrp_routing.constraint_info()` reports exactly this
table from the C++ components (`kDefaultLevel` / `kDefaultWeight`). Every
constraint is flippable (`hard=True` / `hard=False`), none is a reward, and every
one at a weight above 0 is scored by default (`default_enabled`); the three opt-in
objectives (weight 0) are measured but move no score until given a weight. The
route-policy constraints cost 0 while their vehicle field is 0. The four core rules
(`vehicle_capacity_cost`, `max_end_time_cost`, `unassigned_visits_cost`,
`travel_time_cost`) are also the monolithic `ScoreCalculator` and the incremental
calculator, which read the same constants.

**The medium level.** `unassigned_visits_cost` is the domain's medium level. The
domain declares its fold once (`kMediumFold = intoSoft(10^9)` in `domain.hpp`):
the engine compares `HardSoftScore`, so medium lands in soft as
`medium * 10^9 + soft`, which keeps the 3-level `Score` lexicographic. Its weight
is in medium units (default 1). `hard=True` charges one hard unit per penalty
minute; `hard=False` one soft unit per penalty minute; an explicit weight stays in
medium units. `constraint_costs()` carries
`"medium_fold": {"kind": "into_soft", "unit": 1e9}` on every entry, so
`vrp_routing.constraints.score()` folds exactly like the engine. A plugin-path
solve (`constraints=`) reports `plan.score` and every `on_new_best` score from
the calculator's three levels (`levelScores()`), not by decoding the folded soft,
so a soft total of 10^9 or more stays on soft in both.

**Exponent.** It shapes each unit's raw cost before the weight: per vehicle for
the unit constraints, on the single total for `unassigned_visits_cost`.

**0.3.0:** every constraint takes two more knobs, `offset` and `clip` (keyword-only), and
may be picked several times. A raw cost `x` (per vehicle, or the unassigned total) is
shaped as `sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which lowers the penalty of
every vehicle below the offset, an idle one included; the unassigned pool is no vehicle
and is never charged); `offset=0` is the 0.2.0 shaping. Each pick is its own instance with
its own knobs, and the instances add up:

```python
from vrp_routing.constraints import TravelTimeCost, UnassignedVisitsCost, VehicleCapacityCost

vrp.Solver(config).solve_with_constraints(plan, [
    VehicleCapacityCost(),
    UnassignedVisitsCost(),
    TravelTimeCost(),                                         # driving seconds
    TravelTimeCost(weight=1e-9, exponent=3, offset=7200),     # + 1e-9 x (seconds beyond 2 h)^3
])
```

`vrp.constraint_costs(plan, picks)` keys the second pick `travel_time_cost#2`; its
per-vehicle entries list the vehicles they evaluate as `"units"`.

**0.2.0.** Every constraint name ends in `_cost` (C++ struct, header file, registry
name, Python spec class, module-level default instance and catalog name):

| 0.1.x name | 0.2.0 name |
| --- | --- |
| `vehicle_capacity` / `VehicleCapacity` | `vehicle_capacity_cost` / `VehicleCapacityCost` |
| `service_finished_after_max_end_time` / `ServiceFinishedAfterMaxEndTime` | `max_end_time_cost` / `MaxEndTimeCost` |
| `maximize_visits_assigned` / `MaximizeVisitsAssigned` | `unassigned_visits_cost` / `UnassignedVisitsCost` |
| `minimize_travel_time` / `MinimizeTravelTime` | `travel_time_cost` / `TravelTimeCost` |
| `max_travel_time` / `MaxTravelTime` | `max_travel_time_cost` / `MaxTravelTimeCost` |
| `max_route_duration` / `MaxRouteDuration` | `max_route_duration_cost` / `MaxRouteDurationCost` |
| `vehicle_end_time` / `VehicleEndTime` | `vehicle_max_end_time_cost` / `VehicleMaxEndTimeCost` |
| `max_stops` / `MaxStops` | `max_stops_cost` / `MaxStopsCost` |
| `max_route_radius` / `MaxRouteRadius` | `max_route_radius_cost` / `MaxRouteRadiusCost` |
| `minimize_enclosing_circle_radius` / `MinimizeEnclosingCircleRadius` | `enclosing_circle_radius_cost` / `EnclosingCircleRadiusCost` |
| `min_capacity_utilization` / `MinCapacityUtilization` | `min_capacity_utilization_cost` / `MinCapacityUtilizationCost` |
| `minimize_route_duration` / `MinimizeRouteDuration` | `route_duration_cost` / `RouteDurationCost` |
| `minimize_latency` / `MinimizeLatency` | `latency_cost` / `LatencyCost` |
| `vehicle_fixed_cost` / `VehicleFixedCost` | unchanged |

`service_finished_after_max_end_time` aligns with the deadline family as
`max_end_time_cost`, and the vehicle's own end bound becomes
`vehicle_max_end_time_cost` so the two do not clash. The model FIELDS keep their
names (`max_travel_time`, `max_route_duration`, `max_end_time`, `max_stops`,
`max_route_radius_meters`, `min_load_ratio`, `fixed_cost`,
`unassigned_penalty_minutes`). Other changes: `unassigned_visits_cost` defaults to
weight 1 in medium units (it was 10^9 in soft units); `hard=False` on it now demotes
it to soft (it used to be a no-op relabel) and `hard=True` charges 1 hard unit per
penalty minute (it used to charge 10^9); the `enclosing_circle_radius_cost` default
instance no longer passes `weight=0` (the C++ default is 0); `get_constraints()` and
`score()` now refuse a constraint named twice, as the C++ solve path already did (0.3.0
lifts that refusal again, see below);
`constraint_info()` / `ConstraintInfo` are new.
Raw costs and the default score are unchanged.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **LIST**; the domain solver that configures the
engine is `cpp/include/vrp/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** `entityDifficulty` is already set in `cpp/src/solver.cpp` (tightest time window and largest demand first). Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/vrp/constraints/` (currently `vehicle_capacity_cost`, `max_end_time_cost`,
`travel_time_cost`, `max_travel_time_cost`, `max_route_duration_cost`,
`vehicle_max_end_time_cost`, `max_stops_cost`, `max_route_radius_cost`,
`enclosing_circle_radius_cost`, `min_capacity_utilization_cost`, `route_duration_cost`,
`latency_cost`, `vehicle_fixed_cost`, `unassigned_visits_cost`; plus `common.hpp`, the
shared prefix sums in `route_sums.hpp` and the enclosing-circle helpers in
`route_geometry.hpp`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/vrp/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for one of the core rules, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `vrp_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `vrp_plugin_test` (`verifyRawCostsByHand`) and the catalog
   row to `verifyCatalog`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/vrp_routing/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=[..., MyRuleCost(weight=5, hard=True)]`.

The CVRP benchmark scores plain distance, so a new cost does not change it.

## Benchmark

[`benchmarks/vehicle-routing`](../../benchmarks/vehicle-routing) compares this use case with
the OR-Tools Routing solver on the same instances, rescored by the same evaluator.
