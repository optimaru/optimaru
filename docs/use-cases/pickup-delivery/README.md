# Pickup and Delivery with Time Windows (PDPTW)

Extends this repository's vehicle-routing translation, itself derived from the Timefold
Quickstarts `vehicle-routing` example (Apache-2.0); see the repository [NOTICE](../../../NOTICE).

A transport **request** is a pickup and a dropoff that **one** vehicle must
serve, **pickup first**, without ever exceeding its capacity in between.

That coupling is the entire difference from [vehicle-routing](../vehicle-routing),
and it is why this is a separate use case rather than one more constraint bolted
onto that one: a VRP visit is independent and can be moved anywhere, while a PDP
stop drags its partner along. Every part of the solver that differs — the
constraints, the moves, the ruin operators — follows from that one fact.

Uses adaptive large-neighborhood search, running on the generic
engine in [`optimaru/cpp/optimaru/`](../../../optimaru/cpp/optimaru).
The Python API is in [`python.md`](python.md).

```
optimaru/pickup_delivery/        the Python API: solve, models, constraint specs, stubs
optimaru/pickup_delivery/cpp/    the solver (domain, constraints, engine wiring, demo) and the pybind11 bindings
tests/pickup_delivery/           pytest suite; cpp/ holds the C++ tests
```

## Usage

```sh
OPTIMARU_USE_CASES=pickup_delivery uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/pickup_delivery
```

```python
from optimaru import pickup_delivery as pdp

plan = pdp.build_random_instance(request_count=20, vehicle_count=3, seed=7)
solved = pdp.solve(plan, time_limit_seconds=5.0, random_seed=42)   # solves in place and returns the plan
print(solved.score.hard, solved.score.medium, solved.score.soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`pdp.SolverConfig`, whose fields include `spent_limit_seconds`, `random_seed`, `parallel_solver_count` and `full_assert`; the keywords override a copy of it) and `constraints` (see below). `Solver(config).solve(plan)` and `Solver(config).solve_with_constraints(plan, picks)` are the same solves one level down, on the engine objects; for JSON in/out use `optimaru.pickup_delivery.models.solve_request(...)`, which also takes `constraints=`. The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=pickup_delivery && cmake --build build -j
ctest --test-dir build
```

## Constraints

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
| `request_group_cost` | `RequestGroupCost` | hard | 1000 | grouped requests that are not where most of their `group` is: on another vehicle, or unassigned while the others ride (see [Request groups](#request-groups)) |
| `unassigned_requests_cost` | `UnassignedRequestsCost` | medium | 1 | service minutes (rounded down) of the requests left entirely unrouted |

That is the `list_constraints()` order: the sixteen per-vehicle unit constraints
(each vehicle, plus the unassigned pool as the last unit; only `required_vehicle_cost`
and `squared_order_age_cost` price the pool, the others evaluate the vehicles alone),
then the five request-level globals. `optimaru.pickup_delivery.constraint_info()` reports exactly this table
from the C++ components (`kDefaultLevel` / `kDefaultWeight`). Every constraint is
flippable onto any of the three levels (`level="hard"`, `"medium"` or `"soft"`, or a
`Level`), none is a reward, and every one at a weight above 0 is scored by default
(`default_enabled`); the four opt-in objectives
(weight 0) are measured but move no score until given a weight. The route-policy
constraints cost 0 while their vehicle field is 0. The seven core rules
(`vehicle_capacity_cost`, `max_end_time_cost`, the three coupling rules,
`unassigned_requests_cost`, `travel_time_cost`) are also the monolithic
`ScoreCalculator`, which reads the same constants.

**The medium level.** `unassigned_requests_cost` is the domain's medium level. The
domain declares its fold once (`kMediumFold = intoSoft(10^9)` in `domain.hpp`):
the engine compares `Score`, so medium lands in soft as
`medium * 10^9 + soft`, which keeps the 3-level `Score` lexicographic. Its weight
is in medium units (default 1, the same as the CP-SAT reference's). `level="hard"`
charges one hard unit per minute; `level="soft"` one soft unit per minute, in both
solvers; an explicit weight is in units of the level in effect. Any other constraint
may move onto medium the same way (`TravelTimeCost(level="medium")` charges one medium
unit per driving second, in both solvers too), and a repeated pick carries its own
level. `constraint_costs()` carries
`"medium_fold": {"kind": "into_soft", "unit": 1e9}` on every entry, so
`optimaru.pickup_delivery.constraints.score()` folds exactly like the engine. A solve with
`constraints=` reports `plan.score` and every `on_new_best` score with the three
levels read before the fold, so a soft total of 10^9 or more stays on the soft
level.

**Exponent.** It shapes each unit's raw cost before the weight: per vehicle for
the unit constraints, on the single total for the four globals.
`squared_order_age_cost` squares per request inside its raw cost, so an exponent
compounds it (exponent 2 gives a 4th power).

Besides `weight`, `level` and `exponent`, every constraint takes `offset` and `clip`
(keyword-only), and may be picked several times. A raw cost `x` (per vehicle, or a global's total) is shaped
as `sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which lowers the penalty of
every evaluated unit below the offset, an idle vehicle included; the unassigned pool is no
vehicle and only `required_vehicle_cost` and `squared_order_age_cost` evaluate it);
`offset=0` is the plain shaping. Both apply per evaluated unit, not to the total.
`clip=False` with an offset is refused on hard (by default, every constraint the table
lists as hard), because hard 0 must mean no violation; medium folds into soft here, so it
takes the literal formula. Each pick is its own instance with its own knobs, and the
instances add up:

```python
from optimaru.pickup_delivery.constraints import TravelTimeCost, get_constraints

pdp.solve(plan, time_limit_seconds=5.0, random_seed=42, constraints=[
    *get_constraints(),                                        # the defaults
    TravelTimeCost(weight=0.001, exponent=2, offset=7200),     # + 0.001 x (seconds beyond 2 h)^2
])
```

`pdp.constraint_costs(plan, picks)` keys the second pick `travel_time_cost#2`; its
per-vehicle entries list the vehicles they evaluate as `"units"`. The CP-SAT reference
takes integral offsets on every plugin; it refuses a non-integral one.

`squared_order_age_cost` is the squared order age, not lateness past a due time.
`max_end_time_cost` bounds a request's service end, `vehicle_max_end_time_cost` a
vehicle's own end. The model fields keep plain names (`max_stops`, `pinned_stops`,
`required_vehicle`, `group`, `max_travel_time`, `max_route_duration`, `max_end_time`,
`max_route_radius_meters`). `unassigned_requests_cost` defaults to weight 1 in medium
units; `level="soft"` on it moves it to soft in the engine as in CP-SAT, and
`level="hard"` charges 1 hard unit per minute.

Routes are **open by default**: a vehicle ends its day at its last stop and the
leg back home is neither driven nor added to the route end time. Set
`Vehicle::returnToDepot` (`return_to_depot` in the Python models) to charge
and time a classic round trip; the public Li & Lim benchmark does.

The JSON contract (`optimaru.pickup_delivery.models.VehicleModel`) also caps each vehicle at
**20 stops by default** (`max_stops`, a request counts as two); pass `0` to lift
it. The C++ `Vehicle` and the pybind constructor keep `maxStops = 0` (disabled),
so hand-built plans are unaffected.

`PdpSolution.vehicle_metrics` reports per-vehicle stops, requests, peak load,
delivered demand, driving / waiting / service seconds and the route end time.
They are post-processed from the finished routes (`optimaru.pickup_delivery.models.
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
and `request_group_cost` use the same weight. `pinned_stops_cost` uses `ScoreCalculator::kPinnedPenalty`
(1e12), far above what the other hard rules total on real input, so no trade
between them moves a pinned stop. If a result breaks a pin anyway, its `score`
shows it in that unit. The CP-SAT reference enforces the three coupling rules
structurally and refuses any level but hard (`level="medium"` or `"soft"`) or another
weight for them; the engine keeps them flippable.

## Searching in whole requests

The engine's built-in moves are element-granular: they relocate, swap and
reverse individual stops. On a PDP almost every such move splits a request or
puts a dropoff before its pickup, and the acceptor rejects it. So the solver
registers operators whose unit is the request: a pickup and its dropoff leave a
route together and come back together.

### Where a request fits

`pdp/request_insertion.hpp` answers one question: the cheapest feasible way to
add a request to a route. For each pickup slot it walks the dropoff slot forward
and carries the schedule along. A `RouteTimeline` keeps, per stop, the departure
time, the latest arrival that still keeps the rest of the route on time and the
load on board, so one slot pair is checked in O(1) and a route of n stops in
O(n²).

Feasible is what the plan says: the running load against the capacity, every
stop's window, and the vehicle limits that are set (`max_end_time`,
`max_route_duration`, `max_travel_time`, `max_stops`, the pinned stops, a
request's required vehicle). Among the feasible slot pairs the one that adds the
least driving wins. The objective still has the last word: an operator applies
what the scan proposes through the engine, and the acceptor judges the real
score. A constraint selection that prices other things (a fixed cost, the route
duration) is therefore followed, only less directly.

The scan does not read the selection, though. The request moves only propose
placements that keep the plan's windows, capacity and vehicle limits, whatever
level those constraints are scored on. With one of them soft or left out, a
request that has no slot inside the limits is not placed by a request move:
the engine's stop moves are what try plans that break a limit.

A route that already breaks a limit takes less: no request in front of a stop
that is late, and none at all while it is over its travel budget or its stop
cap. Requests can still be taken off such a route, which is how the request
moves mend one; a plan that cannot be made feasible is left to the engine's
stop moves, which trade one violation against another.

Construction (`constructPairs`) places every request with the same scan, so the
first plan already keeps the vehicle limits.

### The request moves

`pdp/request_moves.hpp` registers three move operators on that scan. They work
on copies of the routes and write a changed route through the engine once, when
the whole move is laid out, so a candidate never holds half a request.

| Operator | One application |
| --- | --- |
| `requestRuinRecreate` | Takes 1 to `request_ruin_max_requests` requests out (never more than two fifths of the plan's requests; a request of a group brings its whole group, which can take the count past that) and reinserts each at its cheapest slot pair: large neighbourhood search at request granularity. Which requests: random ones, related ones (the requests of nearby stops), the ones whose stops cost the most driving, the requests of one route, or the requests in adjacent stretches of nearby routes. In which order: at random, or by regret-`regret_k` (the request that loses the most by waiting goes first). `blink_rate` skips a candidate slot now and then, so the same hole is not always rebuilt the same way. Up to two requests waiting in the unassigned pool join each reinsertion, which is how a left-out request gets routed. |
| `bundleRelocate` | Moves a bundle (1 to `bundle_max_requests` requests whose pickups follow each other on a route, a group counting as one) to one other route, each request at its cheapest slot there. A cluster of orders crosses in one step; moved one by one, the plans in between are worse or infeasible. |
| `bundleExchange` | Swaps a bundle of one route with a bundle of a nearby route, the two sized independently. |

A request with a pinned stop is left where it is, and a request bound to a
vehicle is only offered that vehicle. The requests of a group
([Request groups](#request-groups)) are one unit in all three: they are taken
out together and put back on one vehicle, or left out together. On a large fleet (more than 32 vehicles
to offer) a request is offered to the idle vehicles and to the routes that hold
one of its nearby stops, and to the rest of the fleet only when none of those
has room; idle vehicles that would serve it identically are offered once.

The removal and reinsertion rules follow Shaw (1998), Ropke & Pisinger (2006,
"An Adaptive Large Neighborhood Search Heuristic for the Pickup and Delivery
Problem with Time Windows") and Christiaens & Vanden Berghe (2020, "Slack
Induction by String Removals for Vehicle Routing Problems").

### The pair operators

The older pair operators stay registered. The engine's ruin-and-recreate kick
rebuilds a plan one stop at a time and knows nothing of pairs, and two of them
deal with what it leaves behind.

**`PairRelocateMove`** moves a whole request, in three tiers:

1. **Repair** — a request whose pair is currently broken (split across
   vehicles, dropoff before pickup, only half routed) is re-laid as a proper
   pair.
2. **Assignment** — a whole request moves into the unassigned pool, or out of it
   at its cheapest feasible slot pair. Moving a *single* stop into the pool
   leaves a half-assigned request, which is hard-infeasible and always rejected.
3. **Relocate** — to a position anchored on a nearby stop.

**`PairExchangeMove`** swaps two requests of different routes in place (from
`pair_exchange_min_nodes` stops on).

**`PairRuin`** makes the engine's kick take whole requests, seeded at a random
one and spreading to its nearest neighbours. The built-in ruins pick individual
stops, which strands half a request.

These work one request at a time, so with [request groups](#request-groups) they
stay inside a group: the pair move takes a grouped request only to the vehicle
another request of its group rides (out of the pool to join them, never to ride
alone) and drops it only when none of them rides, the exchange leaves grouped
requests out, and the ruin takes a group whole.

### What they are worth

Three configurations on the same generated instances (`build_random_instance`
with the capacity and the windows below, closed routes; 5 seeds each, 4 s): the
engine's stop-granular moves alone (`pair_moves`, `pair_ruin`,
`request_ruin_recreate` and `bundle_moves` off), with the pair operators, and
with the request moves as well (the defaults). Cells are the mean medium / soft
level; measured 2026-11-11.

| Instance | stop-granular moves | + pair operators | + request moves |
| --- | --- | --- | --- |
| loose (50 req / 6 veh, cap 12, 300 min windows) | 0 / −11,994 | 0 / −10,472 | 0 / −9,482 |
| medium (50 / 4, cap 8, 180 min) | 0 / −14,153 | 0 / −12,770 | 0 / −11,279 |
| tight (60 / 3, cap 6, 120 min) | −144 / −14,891 | −130 / −14,619 | −86 / −11,765 |
| very tight (60 / 3, cap 5, 90 min) | −268 / −10,686 | −254 / −11,747 | −218 / −8,594 |

The request moves give the better plan in all 20 runs, against either other
configuration. Where every request fits (loose, medium) they drive about a
tenth less than the pair operators do. Where capacity and windows force
requests to be left out they route more of them, which is the medium level, and
drive less for the ones they route. The pair operators alone mostly buy
coverage: they beat the stop-granular moves in 17 of the 20 runs.

Most of the gain is `requestRuinRecreate`. On 14 of the harder Li & Lim
instances (3 s each, 5 seeds) the mean distance ends 0.5% above the best known
with the defaults and 4.0% above with `request_ruin_recreate` off. Switching off
the bundle moves, the pair operators, regret or blinking instead leaves it
between 0.1% and 0.5%, which is the spread between seeds; without the engine's
kick it is 0.7%. The Li & Lim results at 30 s are in
[`benchmarks/pickup-delivery`](../../../benchmarks/pickup-delivery/cpsat/PUBLIC_BENCHMARKS.md).

## Scoring: two implementations, checked against each other

`ScoreCalculator::score` is the full recompute and the readable
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
coupling penalty; `ScoreChecks::Full` caught it, and `pdp_test` now covers it
directly, including the transient half-assigned states the engine passes
through mid-move.

## Build & run

```sh
cmake -B build -DOPTIMARU_USE_CASES=pickup_delivery && cmake --build build -j
./build/pickup_delivery/pdp_demo                       # 30 requests, 5 vehicles, 5s
./build/pickup_delivery/pdp_demo 60 6 10               # 60 requests, 6 vehicles, 10s
./build/pickup_delivery/pdp_demo 60 6 10 --stop-moves-only   # control: the engine's stop moves only
./build/pickup_delivery/pdp_test
./build/pickup_delivery/pdp_plugin_test    # assembly == monolith, raw costs by hand, catalog, medium level
./build/pickup_delivery/pdp_request_moves_test   # insertion scan == brute force, the request moves in a solve
```

```sh
OPTIMARU_USE_CASES=pickup_delivery uv sync && uv run pytest -q tests/pickup_delivery
```

## Request groups

Orders the input has already merged into one job get the same `group`: a name on
`DeliveryModel` (`None` or an empty name is no group), `Delivery.group_index` on
a plan (-1 for none). The requests of a group ride **one vehicle together, or all
stay unassigned**. A group holds any
number of requests and a request belongs to at most one. Their stops stay
ordinary stops: the vehicle visits them in any order that keeps each pickup
before its dropoff, with other requests' stops in between.

```python
DeliveryModel(id="order-17", group="job-4", demand=1, pickup=..., dropoff=...)
DeliveryModel(id="order-18", group="job-4", demand=2, pickup=..., dropoff=...)
```

`request_group_cost` (hard, weight 1000) scores it. A request is where its pickup
is, on a vehicle or on none, and the raw cost is the number of grouped requests
that are not where most of their group is. A group of three with one request on
another vehicle costs 1, and so does one with a request left unassigned while two
ride; spread over three vehicles it costs 2. Every request that joins the others
takes 1 off, which is the slope the stop moves follow back to a whole group.

The search moves a group as one unit. Construction puts its requests on the one
vehicle that takes all of them for the least added driving, and the request moves
take a group out of its route together and put it back on one vehicle, each
request at its cheapest slot pair once the earlier ones are in. When no vehicle
has room for the whole group (too many stops for `max_stops`, windows one route
cannot keep) all of it stays in `unassigned`, and the result is still feasible:
the group is not routed in parts. The requests of a group go onto a vehicle in
input order, each at its cheapest slot pair behind the earlier ones, and no other
order is tried. One such offer costs a scan of the route per request, so groups
are meant to be a handful of requests: nothing caps the size, but a group of
more than about ten slows every request move that takes it. As with the vehicle limits, the request moves
keep groups whole whatever level the constraint is scored on; with it soft or
left out, only the engine's stop moves try plans that split one.

A group is bound to a vehicle by any of these:

- `required_vehicle` on one of its requests binds all of them.
- A pinned stop. That request cannot leave its route, so the rest of the group
  joins it there.
- A request already on an `initial_route`. Construction adds the waiting requests
  of the group to that vehicle; the search may then move the whole group. When
  the group starts on several vehicles the waiting requests join the one that
  holds most of it, and the search brings the rest over.

Two requests of a group that are bound to different vehicles, each by its
`required_vehicle` or by a pinned stop, are refused (`ValueError`): such a group
could never be whole.

`solve_request` scores groups whenever a request names one: it then solves on the
constraint assembly, as it does when a vehicle limit is set. `pdp.solve(plan)`
does the same for a plan with a `group_index` set. One level down,
`Solver(config).solve(plan)` (the C++ `Solver::solve(plan)`) scores the seven
core rules, which do not include this one: use `solve_with_constraints` there.
With `pair_construction_heuristic` off the engine's own construction places
stops one at a time and may start with a group split, which the search then has
to mend. Like `required_vehicle_cost`
and `pinned_stops_cost` the constraint is engine-only: the CP-SAT reference has
no plugin for it and does not read `group`.

## Starting from an existing solution

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

```python
from optimaru.pickup_delivery.models import solve_request, with_initial_solution

solution = solve_request(request)                        # request: a PdpRequest
next_request = with_initial_solution(request, solution)  # or solution.model_dump(mode="json")
better = solve_request(next_request)
```

`with_initial_solution` turns a solution into the `initial_route` of each vehicle (above), replacing the ones the request had. Construction places only the requests in no route, so a partial solution works and the `unassigned` ones get another try. With `timelimit=0` a complete solution comes back as it went in.

A vehicle's `pinned_stops` stay as the request has them: its route in the solution must start with them, or `ValueError` is raised. The same goes for an unknown id, a stop routed twice, or a request whose pickup and dropoff are apart, so a hard-infeasible result with a broken pair cannot be used as a start.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **sequence** shape; the domain solver that configures the
engine is `optimaru/pickup_delivery/cpp/include/pdp/solver.hpp` + `optimaru/pickup_delivery/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). `PairRelocateMove` / `PairExchangeMove` (move a pickup together with its drop-off) and the `PairRuin` ruin operator are defined in `optimaru/pickup_delivery/cpp/src/solver.cpp`, the request moves in `optimaru/pickup_delivery/cpp/src/request_moves.cpp` (on the insertion scan of `request_insertion.cpp`) — use them as templates. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/pickup_delivery/cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** Both levers are used: `difficulty` and a domain constructor (`constructPairs`, which inserts each pickup with its drop-off) in `optimaru/pickup_delivery/cpp/src/solver.cpp`; the engine heuristic fills whatever is left. Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`optimaru/pickup_delivery/cpp/include/pdp/constraints/` (currently `vehicle_capacity_cost`, `max_end_time_cost`,
`travel_time_cost`, `max_travel_time_cost`, `max_route_duration_cost`,
`vehicle_max_end_time_cost`, `max_stops_cost`, `max_route_radius_cost`,
`enclosing_circle_radius_cost`, `min_capacity_utilization_cost`, `route_duration_cost`,
`latency_cost`, `squared_order_age_cost`, `vehicle_fixed_cost`, `pinned_stops_cost`,
`required_vehicle_cost`, `paired_request_cost`, `same_vehicle_cost`,
`pickup_before_dropoff_cost`, `request_group_cost`, `unassigned_requests_cost`; plus `common.hpp` and the
enclosing-circle helpers in `route_geometry.hpp`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `optimaru/pickup_delivery/cpp/include/pdp/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for one of the core rules, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `pdp_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `pdp_plugin_test` (`verifyRawCostsByHand`) and the catalog
   row to `verifyCatalog`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `optimaru/pickup_delivery/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=[..., MyRuleCost(weight=5, level="hard")]`;
5. mirror it in the CP-SAT model, `external_solver.pickup_delivery` in
   [external-solver](https://github.com/optimaru/external-solver) (a plugin subclassing the
   spec; defaults come from `constraint_info()`), so the benchmark in
   [`benchmarks/pickup-delivery/cpsat`](../../../benchmarks/pickup-delivery/cpsat)
   stays like for like.

## Benchmark

[`benchmarks/pickup-delivery`](../../../benchmarks/pickup-delivery) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.
