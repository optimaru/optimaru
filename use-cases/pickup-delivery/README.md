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
```

```sh
cd python && uv run python setup.py build_ext --inplace && uv run pytest
```

## The constraints

| Level | Constraint | |
| --- | --- | --- |
| hard | `pairedRequest` | a request is routed whole, or not at all |
| hard | `sameVehicle` | its pickup and dropoff share one vehicle |
| hard | `pickupBeforeDropoff` | and the pickup comes first in that route |
| hard | `vehicleCapacity` | the **running load** never exceeds the capacity |
| hard | `serviceFinishedAfterMaxEndTime` | time-window lateness, in whole minutes |
| medium | `unassignedRequests` | service minutes of the requests left unrouted |
| soft | `minimizeLateness` (opt-in) | squared seconds from each request's `order_time` to the end of its dropoff |
| soft | `minimizeTravelTime` | total driving seconds — home → stops, plus the leg back home only when the vehicle's `returnToDepot` is set |

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

The three coupling rules share one penalty weight,
`ScoreCalculator::kCouplingPenalty`. Any nonzero hard total already means
infeasible, so that weight only shapes the gradient *within* the infeasible
region; it is set well above a typical lateness so the search fixes broken
pairs before polishing time windows.

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

<!-- usage-and-extending -->

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

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
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
`cpp/include/pdp/constraints/` (currently `load_utilization`, `minimize_travel_time`, `paired_request`, `pickup_before_dropoff`, `route_costs`, `route_geometry`, `route_limits`, `same_vehicle`, `service_finished_after_max_end_time`, `unassigned_requests`, `vehicle_capacity`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/pdp/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `pdp_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/pdp_routing/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/pickup-delivery/cpsat`](../../benchmarks/pickup-delivery/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/pickup-delivery`](../../benchmarks/pickup-delivery) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.
