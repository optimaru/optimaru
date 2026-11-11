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
                                         # raw costs by hand, catalog, medium level),
                                         # vrp_group_moves_test (visit groups)
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
| `visit_order_cost` | `VisitOrderCost` | hard | 1000 | per vehicle: pairs of its visits served the wrong way round, a visit before one its `after` names (see [Visit groups](#visit-groups)) |
| `visit_group_cost` | `VisitGroupCost` | hard | 1000 | grouped visits that are not where most of their `group` is: on another vehicle, or unassigned while the others ride |
| `unassigned_visits_cost` | `UnassignedVisitsCost` | medium | 1 | penalty minutes of the visits left unrouted (`unassigned_penalty_minutes`, by default the service time in whole minutes) |

That is the `list_constraints()` order: the fourteen per-vehicle unit constraints
(each vehicle; `constraint_costs()` lists the unassigned pool as one more cell, always
0, which no route constraint evaluates), then the two globals, `visit_group_cost` and
`unassigned_visits_cost`. `optimaru.vehicle_routing.constraint_info()` reports exactly this
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

## Visit groups

Orders the input has already merged into one job get the same `group`: a name on
`VisitModel` (`None` or an empty name is no group), `Visit.group_index` on a plan
(-1 for none). The visits of a group ride **one vehicle together, or all stay
unassigned**. A group holds any number of visits and a visit belongs to at most
one. Other visits may be served between them.

`after` puts a **partial order** on a group: it lists the visits of the same
group that are served before this one on the route (`Visit.after_visit_indices`
on a plan). Visits the order does not relate may be served either way round.

```python
VisitModel(id="load", group="job-4", location=...)
VisitModel(id="unload-a", group="job-4", after=["load"], location=...)
VisitModel(id="unload-b", group="job-4", after=["load"], location=...)   # a and b in either order
VisitModel(id="sign-off", group="job-4", after=["unload-a", "unload-b"], location=...)
```

A visit may only come after visits of its own group, the order must have no
cycle, and a visit is named once; anything else is refused (`ValueError`).

Two hard constraints score it, both at weight 1000, well above a typical lateness
or overload, so a broken group is mended before windows are polished:

- `visit_group_cost` counts the grouped visits that are not where most of their
  group is, on a vehicle or on none. A group of three with one visit on another
  vehicle costs 1, and so does one with a visit left unassigned while two ride;
  spread over three vehicles it costs 2. Every visit that joins the others takes
  1 off, which is the slope the engine's one-visit moves follow back to a whole
  group.
- `visit_order_cost` counts, per vehicle, the pairs of its visits that are served
  the wrong way round. A pair whose visits ride different vehicles is left to
  `visit_group_cost`.

The search moves a group as one unit. Before the engine's construction runs,
each group is placed whole on the vehicle that takes all of its visits for the
least added driving, every visit at its cheapest slot behind the ones it comes
after (`constructGroups`); a group no vehicle takes is parked unassigned, whole.
A domain move operator, `groupRuinRecreate` (`vrp/group_moves.hpp`), then takes a
group out of its route, alone or with up to `group_ruin_max_visits` more visits
(the groups and single visits of nearby stops, or other groups), and puts each
unit back on one vehicle, the larger units first. That is how a group changes
vehicle, how groups trade places, and how a waiting group gets routed; the
engine's own moves reorder a group inside its route and are priced by the two
constraints when they would split one. The operator is registered only for a
plan with groups, so a plan without them is solved exactly as before.

The slots it offers are the ones the plan allows: the vehicle's capacity, every
visit's window, and the vehicle limits that are set (`max_end_time`,
`max_route_duration`, `max_travel_time`, `max_stops`), whatever level those
constraints are scored on. The visits of a group go onto a vehicle in an order
that respects `after`, each at its cheapest slot once the earlier ones are in;
one such offer costs a walk of the route per visit, so groups are meant to be a
handful of visits, not dozens.

What the operator is worth, on generated instances of 90 visits around one depot
with morning or afternoon windows, visits in groups of three picked at random
(4 s, 5 seeds, mean driving seconds; every group whole and in order in all 60
results):

| Instances | with `groupRuinRecreate` (default) | the engine's moves alone | nothing grouped |
| --- | --- | --- | --- |
| half the visits grouped | 8,986 | 10,760 | 6,256 |
| half grouped, each group a chain | 9,769 | 11,182 | 6,256 |
| every visit grouped, each group a chain | 12,647 | 15,709 | 6,256 |
| half grouped in chains, a tighter fleet | 10,185 | 11,277 | 6,447 |

The operator drives 10% to 20% less and is better in 19 of the 20 pairs. The last
column is what such groups cost: three visits picked at random across town and
tied to one vehicle pull every route across town, so these plans drive 44% to
102% more than the same visits left free. Groups whose visits lie near each
other cost far less. A grouped plan also settles early: on two of these instances
a 60 s solve ends within 2% of a 4 s one, while nine seeds end 3% and 6% apart
(10 s), so another seed buys more than a longer run.

Every entry point scores a grouped plan with the default constraints, since the
four core rules include neither of these two: `solve_request` when a visit names
a group, and `vrp.solve(plan)`, `Solver(config).solve(plan)` and the C++
`Solver::solve(plan)` when a visit has a `group_index`. That is a solve on the
constraint assembly, as when a vehicle limit is set: the whole default set is
scored, and `num_workers` / `parallel_solver_count` have no effect. Both
constraints are engine-only: the CP-SAT reference has no plugin for them and
reads neither `group` nor `after`.

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
(weights in `Solver::Config`). The one domain move is `groupRuinRecreate` in `optimaru/vehicle_routing/cpp/src/group_moves.cpp` (on the insertion scan of `visit_insertion.cpp`), registered for plans with [visit groups](#visit-groups) — use it as a template. A new move is a
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
`latency_cost`, `vehicle_fixed_cost`, `visit_order_cost`, `visit_group_cost`,
`unassigned_visits_cost`; plus `common.hpp`, the
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
