# Vehicle Routing

Derived from the Timefold Quickstarts `vehicle-routing` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../../NOTICE).

Domain model, constraints and API details: [`python.md`](python.md) and [`optimaru/vehicle_routing/cpp/`](cpp.md).

## Usage

```sh
OPTIMARU_USE_CASES=vehicle_routing uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/vehicle_routing
```

```python
from optimaru import vehicle_routing as vrp

locations = [vrp.Location(37.5665, 126.9780),   # index 0: vehicle home
             vrp.Location(37.5700, 126.9820)]   # index 1: a customer
vehicles = [vrp.Vehicle("V1", capacity=10, home_location_index=0, departure_time=7 * 3600)]
visits = [vrp.Visit("C1", "Customer 1", location_index=1, demand=3,
                    min_start_time=8 * 3600, max_end_time=18 * 3600, service_duration=600)]
plan = vrp.VehicleRoutePlan("demo", locations, vehicles, visits)

solved = vrp.solve(plan, time_limit_seconds=5.0, random_seed=42)   # solves in place and returns the plan
print(solved.score, solved.routes())            # 0hard/0medium/-38soft {'V1': ['C1']}
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`vrp.SolverConfig`, whose fields include `spent_limit_seconds`, `random_seed`, `parallel_solver_count` and `full_assert`; the keywords override a copy of it) and `constraints` (see below). `Solver(config).solve(plan)` and `Solver(config).solve_with_constraints(plan, picks)` are the same solves one level down, on the engine objects; for JSON in/out use `optimaru.vehicle_routing.models.solve_request(...)`, which also takes `constraints=`. The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=vehicle_routing && cmake --build build -j
ctest --test-dir build   # vrp_test, vrp_plugin_test (assembly == monolith,
                                         # raw costs by hand, catalog, medium level)
```

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
0, which no route constraint evaluates), then the global `unassigned_visits_cost`. `optimaru.vehicle_routing.constraint_info()` reports exactly this
table from the C++ components (`kDefaultLevel` / `kDefaultWeight`). Every
constraint is flippable onto any of the three levels (`level="hard"`, `"medium"` or
`"soft"`, or a `Level`), none is a reward, and every one at a weight above 0 is scored
by default (`default_enabled`); the three opt-in objectives (weight 0) are measured
but move no score until given a weight. The route-policy constraints cost 0 while their
vehicle field is 0. The four core rules
(`vehicle_capacity_cost`, `max_end_time_cost`, `unassigned_visits_cost`,
`travel_time_cost`) are also the monolithic `ScoreCalculator` and the incremental
calculator, which read the same constants.

**The medium level.** `unassigned_visits_cost` is the domain's medium level. The
domain declares its fold once (`kMediumFold = intoSoft(10^9)` in `domain.hpp`):
the engine compares `Score`, so medium lands in soft as
`medium * 10^9 + soft`, which keeps the 3-level `Score` lexicographic. Its weight
is in medium units (default 1). `level="hard"` charges one hard unit per penalty
minute; `level="soft"` one soft unit per penalty minute; an explicit weight is in units
of the level in effect. Any other constraint may move onto medium the same way
(`TravelTimeCost(level="medium")` charges one medium unit per driving second), and a
repeated pick carries its own level. `constraint_costs()` carries
`"medium_fold": {"kind": "into_soft", "unit": 1e9}` on every entry, so
`optimaru.vehicle_routing.constraints.score()` folds exactly like the engine. A plugin-path
solve (`constraints=`) reports `plan.score` and every `on_new_best` score from
the calculator's three levels (`levelScores()`), not by decoding the folded soft,
so a soft total of 10^9 or more stays on soft in both.

**Exponent.** It shapes each unit's raw cost before the weight: per vehicle for
the unit constraints, on the single total for `unassigned_visits_cost`.

**Offset, clip and repeats.** Every constraint takes the knobs `weight`, `level`,
`exponent`, `offset` and `clip` (`offset` and `clip` keyword-only), and may be picked
several times. A raw cost `x` (per vehicle, or the unassigned total) is
shaped as `sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which lowers the penalty of
every vehicle below the offset, an idle one included; the unassigned pool is no vehicle
and is never charged); `offset=0` is the plain power. Both apply per vehicle, not to the
total. `clip=False` with an offset is refused on hard (by default, every constraint the
table lists as hard), because hard 0 must mean no violation; medium folds into soft here,
so it takes the literal formula. A negative offset with `clip=True`, such as
`TravelTimeCost(offset=-60)`, is a fixed charge of 60 per vehicle that drives; the engine
recomputes it for every route a move changes, so the solve takes about half as many steps
per second. Each pick is its own instance with its own knobs, and the instances add up:

```python
from optimaru.vehicle_routing.constraints import TravelTimeCost, UnassignedVisitsCost, VehicleCapacityCost

vrp.solve(plan, time_limit_seconds=5.0, random_seed=42, constraints=[
    VehicleCapacityCost(),
    UnassignedVisitsCost(),
    TravelTimeCost(),                                         # driving seconds
    TravelTimeCost(weight=1e-9, exponent=3, offset=7200),     # + 1e-9 x (seconds beyond 2 h)^3
])
```

`vrp.constraint_costs(plan, picks)` keys the second pick `travel_time_cost#2`; its
per-vehicle entries list the vehicles they evaluate as `"units"`.

**Names.** Every constraint name ends in `_cost` (C++ struct, header file, registry
name, Python spec class, module-level default instance and catalog name); in
`vehicle_fixed_cost` that ending is the rule's own subject (the vehicle's fixed cost),
not an added suffix. `max_end_time_cost` is the visit deadline and
`vehicle_max_end_time_cost` the vehicle's own end bound. The model FIELDS keep their plain
names (`max_travel_time`, `max_route_duration`, `max_end_time`, `max_stops`,
`max_route_radius_meters`, `min_load_ratio`, `fixed_cost`, `unassigned_penalty_minutes`).
`unassigned_visits_cost` defaults to weight 1 in medium units: `level="soft"` demotes it to
soft and `level="hard"` charges 1 hard unit per penalty minute. `enclosing_circle_radius_cost`
defaults to weight 0 (opt-in).

## Starting from an existing solution

```python
from optimaru.vehicle_routing.models import solve_request, with_initial_solution

solution = solve_request(request)                        # request: a RoutePlanRequest
next_request = with_initial_solution(request, solution)  # or solution.model_dump(mode="json")
better = solve_request(next_request)
```

`with_initial_solution` returns a new request whose vehicles carry `initial_route`, a list of visit ids. The solve keeps those routes as its starting point and construction places only the visits in no route, so a partial solution works and the `unassigned` visits get another try. With `timelimit=0` a complete solution comes back as it went in. An unknown id or a visit routed twice raises `ValueError`.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **sequence** shape; the domain solver that configures the
engine is `optimaru/vehicle_routing/cpp/include/vrp/solver.hpp` + `optimaru/vehicle_routing/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/vehicle_routing/cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** `difficulty` is already set in `optimaru/vehicle_routing/cpp/src/solver.cpp` (tightest time window and largest demand first). Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`optimaru/vehicle_routing/cpp/include/vrp/constraints/` (currently `vehicle_capacity_cost`, `max_end_time_cost`,
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
2. include it in `optimaru/vehicle_routing/cpp/include/vrp/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for one of the core rules, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `vrp_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `vrp_plugin_test` (`verifyRawCostsByHand`) and the catalog
   row to `verifyCatalog`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `optimaru/vehicle_routing/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=[..., MyRuleCost(weight=5, level="hard")]`.

The CVRP benchmark scores plain distance, so a new cost does not change it.

## Benchmark

[`benchmarks/vehicle-routing`](../../../benchmarks/vehicle-routing) compares this use case with
the OR-Tools Routing solver on the same instances, rescored by the same evaluator.
