# Vehicle Routing — C++ port

A C++17 port of the optimization **core** of the `vehicle-routing`
Java example model (Apache-2.0; see the repository NOTICE): the domain model, the scoring (HardMediumSoftScore) and the
solve, running on the generic engine in [`include/optimaru/`](../../../include/optimaru).

Scope: optimization core only — no REST layer, no web UI.

## Build & run

```sh
cmake -B build && cmake --build build
./build/vrp_demo                 # default: 25 visits, 6 vehicles, 3s
./build/vrp_demo 77 6 5          # 77 visits, 6 vehicles, 5s
./build/vrp_demo 77 6 5 --no-alns # ruin & recreate off, as a control
./build/vrp_test                 # Java fixtures + engine integration
```

## How it maps to the Java code

| Java (reference model)                                      | C++ port                                        |
| ----------------------------------------------------------- | ----------------------------------------------- |
| `@PlanningSolution VehicleRoutePlan`                        | `struct VehicleRoutePlan` (`domain.hpp`)        |
| `@PlanningEntity Vehicle` + `@PlanningListVariable visits`  | `struct Vehicle` + `std::vector<int> visitIndices` |
| `@PlanningEntity Visit` + shadow vars                       | `struct Visit` (+ derived `vehicleIndex`, `arrivalTime`) |
| `@InverseRelationShadowVariable` / `@PreviousElementShadowVariable` / `arrivalTimeSupplier` | `ScoreCalculator::updateShadowVariables` |
| `HardMediumSoftScore`                                       | `struct Score` (lexicographic compare)          |
| `HaversineDrivingTimeCalculator`                            | `haversine.{hpp,cpp}` (identical formula)       |
| `VehicleRoutingConstraintProvider` (4 constraints)          | `ScoreCalculator` (reference) + `VehicleRoutingIncrementalScoreCalculator` (live) |
| Java engine: Construction Heuristic + Local Search          | `include/optimaru/`'s `optimaru::Solver`, wrapped by `Solver` |
| `termination.spent-limit=30s`                               | `Solver::Config::spentLimit`                    |
| test `best-score-limit=0hard/0medium/*soft`                 | `Solver::Config::bestScoreLimit = {0,0,min}`    |

### The four constraints

- **Hard** `vehicleCapacity` — penalize `totalDemand - capacity` when overloaded.
- **Hard** `serviceFinishedAfterMaxEndTime` — penalize lateness (`arrivalTime +
  serviceDuration` past `maxEndTime`), rounded up to whole minutes.
- **Medium** `maximizeVisitsAssigned` — penalize each unassigned visit by
  `Visit::unassignedPenalty()`, which defaults to its service duration in
  minutes (the Java rule) but can be set per visit — see below.
- **Soft** `minimizeTravelTime` — penalize each vehicle's total driving seconds
  (home → visits, plus the leg back home only when `Vehicle::returnToDepot` is
  set; routes are open by default and end at their last visit).

## How it runs on the generic engine

`Solver` (`solver.hpp`) is a thin wrapper that translates the quickstart's
config into a `optimaru::SolverConfig`. The engine then does:

1. **Construction heuristic** — cheapest insertion, in First-Fit-Decreasing
   order (tightest time window and largest demand first).
2. **Local search** — the sampled move mix (relocate / swap / 2-opt / or-opt /
   2-opt\* tail swap / CROSS-exchange), plus a PyVRP-style granular
   best-improvement descent on nearby route pairs. The descent enumerates
   relocate, swap, 2-opt, 2-opt\* and bounded ExchangeNM moves, then applies
   the best candidate through the same acceptor (late acceptance by default).
   It is enabled by `granularDescent` and controlled by
   `granularDescentMaxSegment`, `granularDescentCandidateRoutes`, and
   `descentWeight`.
   An ejection chain (`ejectionChainWeight`) covers what single moves
   cannot reach on a tightly loaded fleet: it inserts an unassigned visit (or
   takes the best-soft relocation that breaks capacity), repairs the breach
   with strictly hard-improving relocations and swaps, and lets the acceptor
   judge the whole chain as one move.
   An iterated local search (`iteratedLocalSearchWeight`) is the main engine
   on large instances: ruin a few visits (`iteratedLocalSearchMinRuin` ..
   `MaxRuin`), repair them, then descend with relocate / swap / 2-opt /
   2-opt\* / CROSS (and Or-opt up to `iteratedLocalSearchMaxSegment`) around
   every touched visit's nearby neighbours until the routes are locally
   optimal, and let the acceptor judge the local optimum. All of these
   strategies score candidates through the calculator's move previews
   (`incremental_calculator.hpp`): O(1) from per-route prefix sums on a plan
   where no visit waits for its window, and one walk of the candidate route
   on a plan with waiting (time windows that open after departure). On the
   plugin path the engine's decomposed calculator previews a move per
   component: the four default components answer in O(pieces) from shared
   per-route prefix sums (`constraints/route_sums.hpp`), the optional policies
   by re-costing the written-out candidate route, so the same strategies run
   on any constraint assembly.
3. **Adaptive ruin & recreate (ALNS)** — destroy-and-repair runs as an
   acceptor-judged move (`ruinRecreateAsMoveWeight`). The unconditional
   stagnation kick is available (`ruinRecreate = true`) but off by default:
   on a tightly loaded fleet the kicked solution is one the search never
   recovers from, and the best stops moving (see `Solver::Config`).

The solver uses adaptive large-neighborhood search with
four operator-selection schemes (roulette wheel, segmented roulette wheel,
α-UCB, random), configurable acceptance criteria, and the ruin operators that
matter on routing problems — SISR substring removal and whole-route removal
alongside random / related / worst. Everything is on `Solver::Config`;
`ruinRecreate = true` turns the stagnation kick on (off by default, see above).

### Two design points worth knowing

**The unassigned pool.** The quickstart sets `allowsUnassignedValues = true`,
but the engine's LIST kind requires every element to live in exactly one list.
So `VehicleRoutePlan` carries one extra virtual list at index
`vehicles.size()` holding the unassigned visits. That is what makes assigning
and unassigning ordinary relocate moves — with genuinely list-less elements the
solver could never drop an unprofitable visit nor recover a dropped one, and
the medium-level constraint would be unreachable.

**`Visit::unassignedPenaltyMinutes`.** The Java rule — penalize an unassigned
visit by its service duration — is degenerate when service durations are 0 or a
few seconds: the penalty rounds to zero and "route nothing" becomes the
optimum. This field states what dropping a visit actually costs, defaulting to
`kUnassignedPenaltyAuto`, which reproduces the Java rule exactly. Real
instances (see the benchmark) set it explicitly.

## Scoring: two implementations, checked against each other

- `ScoreCalculator::calculateScore` — the full recompute, and the readable
  statement of the constraints. Authoritative: `Solver::solve` finishes with it.
- `VehicleRoutingIncrementalScoreCalculator` — what the engine actually calls
  during search, maintaining per-route aggregates so a move costs
  O(route length) instead of O(instance).

They must agree. `vrp_test` checks this directly over 400 random list moves,
and `EnvironmentMode::FULL_ASSERT` re-verifies the incremental score against a
full recompute after *every* move (including the ruin & recreate path).
