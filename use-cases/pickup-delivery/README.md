# Pickup and Delivery with Time Windows (PDPTW)

A transport **request** is a pickup and a dropoff that **one** vehicle must
serve, **pickup first**, without ever exceeding its capacity in between.

That coupling is the entire difference from [vehicle-routing](../vehicle-routing),
and it is why this is a separate use case rather than one more constraint bolted
onto that one: a VRP visit is independent and can be moved anywhere, while a PDP
stop drags its partner along. Every part of the solver that differs — the
constraints, the moves, the ruin operators — follows from that one fact.

Uses adaptive large-neighborhood search, running on the generic
engine in [`include/optimaru/`](../../include/optimaru).

```
cpp/     the solver: domain, constraints, engine wiring, demo, tests
python/  pybind11 bindings (pdp_routing)
```

## Build & run

```sh
cd cpp && cmake -B build && cmake --build build
./build/pdp_demo                       # 30 requests, 5 vehicles, 5s
./build/pdp_demo 60 6 10               # 60 requests, 6 vehicles, 10s
./build/pdp_demo 60 6 10 --no-pair-moves   # control: built-in moves only
./build/pdp_test
./build/pdp_plugin_test    # assembly == monolith, raw costs by hand, catalog, medium level
```

```sh
cd python && uv sync --all-groups --all-extras && uv run pytest -q
```

## Starting from existing routes

A solve keeps any route already on a vehicle as its starting point and places only
the requests no route holds. In the JSON contract that is `initial_route`, a list
of stop ids; both stops of a request must be in it, pickup first.

`pinned_stops` fixes that many leading stops, even in a hard-infeasible result.
Everything after them can be reordered, moved to another vehicle or have stops
inserted between. Pinning a
pickup keeps its request on the vehicle, through `same_vehicle_cost`. Goods already on
board have no pickup left, so give them a pickup at the vehicle's position with no
service time and pin it.

`required_vehicle` keeps a request on a vehicle without fixing its place on the
route, for work that is promised but still free to be reordered. A request with no
feasible slot on that vehicle stays in `unassigned` rather than moving to another
one, and the result is hard-infeasible. Search may still put it on its own vehicle
late, when that costs less than waiting.

## The constraints

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
| --- | --- | --- | --- | --- |
| `vehicle_capacity_cost` | `VehicleCapacityCost` | hard | 1 | per vehicle: the **running load** above the capacity, summed over the stops where it persists |
| `max_end_time_cost` | `MaxEndTimeCost` | hard | 1 | per vehicle: time-window lateness (arrival + service past the stop's `max_end_time`), in whole minutes rounded up |
| `travel_time_cost` | `TravelTimeCost` | soft | 1 | per vehicle: driving seconds — home → stops, plus the leg back home only when the vehicle's `returnToDepot` is set |
| `max_travel_time_cost` | `MaxTravelTimeCost` | hard | 1 | per vehicle: driving seconds above `max_travel_time` (0 = no limit) |
| `max_route_duration_cost` | `MaxRouteDurationCost` | hard | 1 | per vehicle: route duration seconds above `max_route_duration` (0 = no limit) |
| `vehicle_max_end_time_cost` | `VehicleMaxEndTimeCost` | hard | 1 | per vehicle: seconds the route ends after the vehicle's `max_end_time` (0 = no limit) |
| `max_stops_cost` | `MaxStopsCost` | hard | 1 | per vehicle: stops above `max_stops`, a request counting as two (0 = no cap) |
| `max_route_radius_cost` | `MaxRouteRadiusCost` | hard | 1 | per vehicle: meters the farthest stop lies beyond `max_route_radius_meters` from home (0 = no limit) |
| `enclosing_circle_radius_cost` | `EnclosingCircleRadiusCost` | soft | 0 (opt-in) | per vehicle: radius in meters of the smallest circle around its stops (0 with fewer than two) |
| `min_capacity_utilization_cost` | `MinCapacityUtilizationCost` | soft | 1 | per used vehicle: demand short of `capacity * min_load_ratio` (0 = no target) |
| `route_duration_cost` | `RouteDurationCost` | soft | 0 (opt-in) | per used vehicle: seconds from departure to the route end |
| `latency_cost` | `LatencyCost` | soft | 0 (opt-in) | per vehicle: seconds from departure to each stop's arrival, summed |
| `squared_order_age_cost` | `SquaredOrderAgeCost` | soft | 0 (opt-in) | per vehicle: squared seconds from each request's `order_time` to the end of its dropoff (the pool charges a dropoff as finishing at 3600 s) |
| `vehicle_fixed_cost` | `VehicleFixedCost` | soft | 1 | per used vehicle: its `fixed_cost` |
| `pinned_stops_cost` | `PinnedStopsCost` | hard | 1e12 | per vehicle: leading positions that do not hold its `pinned_stops`, in order |
| `required_vehicle_cost` | `RequiredVehicleCost` | hard | 1000 | per list: bound requests off their `required_vehicle` — 1 when unassigned, 2 on another vehicle |
| `paired_request_cost` | `PairedRequestCost` | hard | 1000 | requests routed only half (one stop in a route, the other not) |
| `same_vehicle_cost` | `SameVehicleCost` | hard | 1000 | requests whose pickup and dropoff ride different vehicles |
| `pickup_before_dropoff_cost` | `PickupBeforeDropoffCost` | hard | 1000 | requests whose dropoff comes before the pickup in one route |
| `unassigned_requests_cost` | `UnassignedRequestsCost` | medium | 1 | service minutes (rounded down) of the requests left entirely unrouted |

That is the `list_constraints()` order: the sixteen per-vehicle unit constraints
(each vehicle, plus the unassigned pool as the last unit), then the four
request-level globals. `pdp_routing.constraint_info()` reports exactly this table
from the C++ components (`kDefaultLevel` / `kDefaultWeight`). Every constraint is
flippable (`hard=True` / `hard=False`), none is a reward, and every one at a weight
above 0 is scored by default (`default_enabled`); the four opt-in objectives
(weight 0) are measured but move no score until given a weight. The route-policy
constraints cost 0 while their vehicle field is 0. The seven core rules
(`vehicle_capacity_cost`, `max_end_time_cost`, the three coupling rules,
`unassigned_requests_cost`, `travel_time_cost`) are also the monolithic
`ScoreCalculator`, which reads the same constants.

**The medium level.** `unassigned_requests_cost` is the domain's medium level. The
domain declares its fold once (`kMediumFold = intoSoft(10^9)` in `domain.hpp`):
the engine compares `HardSoftScore`, so medium lands in soft as
`medium * 10^9 + soft`, which keeps the 3-level `Score` lexicographic. Its weight
is in medium units (default 1, the same as the CP-SAT reference's). `hard=True`
charges one hard unit per minute; `hard=False` one soft unit per minute, in both
solvers; an explicit weight stays in medium units. `constraint_costs()` carries
`"medium_fold": {"kind": "into_soft", "unit": 1e9}` on every entry, so
`pdp_routing.constraints.score()` folds exactly like the engine. A solve with
`constraints=` reports `plan.score` and every `on_new_best` score with the three
levels read before the fold, so a soft total of 10^9 or more stays on the soft
level.

**Exponent.** It shapes each unit's raw cost before the weight: per vehicle for
the unit constraints, on the single total for the four globals.
`squared_order_age_cost` squares per request inside its raw cost, so an exponent
compounds it (exponent 2 gives a 4th power).

**0.2.0.** Every constraint name ends in `_cost` (C++ struct, header file, registry
name, Python spec class, module-level default instance and catalog name):

| 0.1.x name | 0.2.0 name |
| --- | --- |
| `vehicle_capacity` / `VehicleCapacity` | `vehicle_capacity_cost` / `VehicleCapacityCost` |
| `service_finished_after_max_end_time` / `ServiceFinishedAfterMaxEndTime` | `max_end_time_cost` / `MaxEndTimeCost` |
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
| `minimize_lateness` / `MinimizeLateness` | `squared_order_age_cost` / `SquaredOrderAgeCost` |
| `vehicle_fixed_cost` / `VehicleFixedCost` | unchanged |
| `pinned_stops` / `PinnedStops` | `pinned_stops_cost` / `PinnedStopsCost` |
| `required_vehicle` / `RequiredVehicle` | `required_vehicle_cost` / `RequiredVehicleCost` |
| `paired_request` / `PairedRequest` | `paired_request_cost` / `PairedRequestCost` |
| `same_vehicle` / `SameVehicle` | `same_vehicle_cost` / `SameVehicleCost` |
| `pickup_before_dropoff` / `PickupBeforeDropoff` | `pickup_before_dropoff_cost` / `PickupBeforeDropoffCost` |
| `unassigned_requests` / `UnassignedRequests` | `unassigned_requests_cost` / `UnassignedRequestsCost` |

`minimize_lateness` was never lateness past a due time: it is the squared order
age, hence `squared_order_age_cost`. `service_finished_after_max_end_time` aligns
with the deadline family as `max_end_time_cost`, and the vehicle's own end bound
becomes `vehicle_max_end_time_cost` so the two do not clash. The model FIELDS keep
their names (`max_stops`, `pinned_stops`, `required_vehicle`, `max_travel_time`,
`max_route_duration`, `max_end_time`, `max_route_radius_meters`). Other changes:
`unassigned_requests_cost` defaults to weight 1 in medium units (it was 10^9 in
soft units), `hard=False` on it now demotes it to soft in the engine as in CP-SAT
(it used to be a no-op relabel), and `hard=True` charges 1 hard unit per minute
(it used to charge 10^9); the `enclosing_circle_radius_cost` default instance no
longer passes `weight=0` (the C++ default is 0); `constraint_info()` /
`ConstraintInfo` are new. Raw costs and the default score are unchanged.

Routes are **open by default**: a vehicle ends its day at its last stop and the
leg back home is neither driven nor added to the route end time. Set
`Vehicle::returnToDepot` (`return_to_depot` in the Python models) to charge
and time a classic round trip; the public Li & Lim benchmark does.

The JSON contract (`pdp_routing.models.VehicleModel`) also caps each vehicle at
**20 stops by default** (`max_stops`, a request counts as two); pass `0` to lift
it. The C++ `Vehicle` and the pybind constructor keep `maxStops = 0` (disabled),
so hand-built plans are unaffected.

`PdpSolution.vehicle_metrics` reports per-vehicle stops, requests, peak load,
delivered demand, driving / waiting / service seconds and the route end time.
They are post-processed from the finished routes (`pdp_routing.models.
vehicle_metrics()`), never scored, and the CP-SAT backend computes them the
same way.

Capacity is a *running peak*, not a route total: load rises at a pickup and
falls at the matching dropoff, so a vehicle can serve far more than its
capacity over a day as long as it never carries too much at once. Two requests
of 6 units each break a capacity of 10 when carried together and fit fine when
carried in sequence — `pdp_test` asserts exactly that.

The three coupling rules share one default weight,
`ScoreCalculator::kCouplingPenalty` (1000; their `kDefaultWeight`). Any nonzero
hard total already means infeasible, so that weight only shapes the gradient
*within* the infeasible region; it is set well above a typical lateness so the
search fixes broken pairs before polishing time windows. `required_vehicle_cost`
uses the same weight. `pinned_stops_cost` uses `ScoreCalculator::kPinnedPenalty`
(1e12), far above what the other hard rules total on real input, so no trade
between them moves a pinned stop. If a result breaks a pin anyway, its `score`
shows it in that unit. The CP-SAT reference enforces the three coupling rules
structurally and refuses `hard=False` or another weight for them; the engine
keeps them flippable.

## Why the pair-aware operators exist

The engine's built-in moves are element-granular: they relocate, swap and
reverse individual stops. On a PDP that is not enough, for two distinct
reasons — so the solver registers two extensions.

**`PairRelocateMove`** moves a whole request, in three tiers:

1. **Repair** — a request whose pair is currently broken (split across
   vehicles, dropoff before pickup, only half routed) is re-laid as a proper
   pair. Needed continuously, because the built-in moves *and* the engine's
   recreate both work one stop at a time and have no notion of a pair.
2. **Assignment** — a whole request moves into or out of the unassigned pool.
   This is the only way the medium level is reachable at all: moving a *single*
   stop into the pool leaves a half-assigned request, which is hard-infeasible
   and therefore always rejected. Without it the set of routed requests is
   frozen at whatever the construction heuristic produced.
3. **Relocate** — the ordinary move, to a position anchored on a nearby stop.

Insertion positions are anchored on spatial neighbours rather than chosen
uniformly, mirroring the engine's nearby-biased relocate; a uniformly random
vehicle and position is rejected almost every time and just burns the step
budget.

**`PairRuin`** ruins whole requests, seeded at a random one and spreading to its
nearest neighbours. The built-in ruins pick individual stops, which strands half
a request in a hard-infeasible state the recreate then has to guess its way out
of.

### What they are worth

Measured across instance tightness (5 seeds each, 4s budget,
`cpp/test/pdp_test.cpp` and the harness in its comments):

| Instance | pair-aware wins | built-ins win |
| --- | --- | --- |
| loose (50 req / 6 veh, cap 12, 300 min windows) | 3 | 2 |
| medium (50 / 4, cap 8, 180 min) | 4 | 1 |
| tight (60 / 3, cap 6, 120 min) | 4 | 1 |
| very tight (60 / 3, cap 5, 90 min) | 3 | 2 |

On loose instances it is a wash — the built-in moves already route every
request, so the two configurations differ only by noise in travel time. The
value shows once capacity and windows force requests to be dropped, and the gap
is on the **medium** level rather than soft: on the tightest seeds the built-ins
leave roughly twice as many requests unrouted (−320 vs −160 medium). That is
the expected shape, and it is the honest claim — these operators buy *coverage*,
not travel time.

## Scoring: two implementations, checked against each other

`ScoreCalculator::calculateScore` is the full recompute and the readable
statement of the constraints. `PdpIncrementalScoreCalculator` is what the
engine calls during search: per-route terms (capacity peak, lateness, driving)
are re-derived per touched route, while the coupling terms are maintained
per *request*, since they read both stops of a request and those may sit in
different routes.

The subtle part is ownership. A composite move re-homes a stop with the
destination list's hook pair, which runs *before* the source list's — so
"release everything that was in my list" has to check that the index still
points at this list, or it strands a stop that is in fact routed. Getting that
wrong produced a score that disagreed with the recompute by exactly one
coupling penalty; `FULL_ASSERT` caught it, and `pdp_test` now covers it
directly, including the transient half-assigned states the engine passes
through mid-move.

## Usage

```sh
cd use-cases/pickup-delivery/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import pdp_routing as pdp

plan = pdp.build_random_instance(request_count=20, vehicle_count=3, seed=7)
config = pdp.SolverConfig()
config.spent_limit_seconds = 5.0
config.random_seed = 42
pdp.Solver(config).solve(plan)                     # solves in place
print(plan.score.hard, plan.score.medium, plan.score.soft)
```

`Solver(config).solve(plan)` works on the engine objects; for JSON in/out use `pdp_routing.models.solve_request(...)`, which also takes `constraints=` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/pickup-delivery/cpp -B build/pickup-delivery && cmake --build build/pickup-delivery -j
ctest --test-dir build/pickup-delivery
```

## Starting from an existing solution

```python
from pdp_routing.models import solve_request, with_initial_solution

solution = solve_request(request)                        # request: a PdpRequest
next_request = with_initial_solution(request, solution)  # or solution.model_dump(mode="json")
better = solve_request(next_request)
```

`with_initial_solution` turns a solution into the `initial_route` of each vehicle (see [Starting from existing routes](#starting-from-existing-routes)), replacing the ones the request had. Construction places only the requests in no route, so a partial solution works and the `unassigned` ones get another try. With `timelimit=0` a complete solution comes back as it went in.

A vehicle's `pinned_stops` stay as the request has them: its route in the solution must start with them, or `ValueError` is raised. The same goes for an unknown id, a stop routed twice, or a request whose pickup and dropoff are apart, so a hard-infeasible result with a broken pair cannot be used as a start.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **LIST**; the domain solver that configures the
engine is `cpp/include/pdp/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). `PairRelocateMove` / `PairExchangeMove` (move a pickup together with its drop-off) and the `PairRuin` ruin operator are defined and registered in `cpp/src/solver.cpp` — use them as templates. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Both levers are used: `entityDifficulty` and a domain constructor (`constructPairs`, which inserts each pickup with its drop-off) in `cpp/src/solver.cpp`; the engine heuristic fills whatever is left. Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/pdp/constraints/` (currently `vehicle_capacity_cost`, `max_end_time_cost`,
`travel_time_cost`, `max_travel_time_cost`, `max_route_duration_cost`,
`vehicle_max_end_time_cost`, `max_stops_cost`, `max_route_radius_cost`,
`enclosing_circle_radius_cost`, `min_capacity_utilization_cost`, `route_duration_cost`,
`latency_cost`, `squared_order_age_cost`, `vehicle_fixed_cost`, `pinned_stops_cost`,
`required_vehicle_cost`, `paired_request_cost`, `same_vehicle_cost`,
`pickup_before_dropoff_cost`, `unassigned_requests_cost`; plus `common.hpp` and the
enclosing-circle helpers in `route_geometry.hpp`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/pdp/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for one of the core rules, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `pdp_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `pdp_plugin_test` (`verifyRawCostsByHand`) and the catalog
   row to `verifyCatalog`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/pdp_routing/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/pickup-delivery/cpsat`](../../benchmarks/pickup-delivery/cpsat) (a plugin
   subclassing the spec; defaults come from `constraint_info()`) so the benchmark stays like
   for like.

## Benchmark

[`benchmarks/pickup-delivery`](../../benchmarks/pickup-delivery) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.
