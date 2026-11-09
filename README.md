# optimaru

A metaheuristic optimization engine for real-world scheduling, routing and
assignment, with 22 ready-to-use models you call from Python.

optimaru comes in a free edition and a paid edition; this repository is the
free, open-source edition.

## Quick start

```bash
pip install optimaru
```

One package with every use case as a subpackage — `optimaru.vehicle_routing`,
`optimaru.school_timetabling`, … — compiled for Linux, macOS and Windows
(Python 3.11+). No C++ toolchain is needed.

```python
from optimaru import vehicle_routing as vrp
from optimaru.vehicle_routing.constraints import (
    TravelTimeCost, UnassignedVisitsCost, VehicleCapacityCost, get_constraints,
)

# Depot at index 0, then three customers.
locations = [vrp.Location(37.5665, 126.9780), vrp.Location(37.5700, 126.9820),
             vrp.Location(37.5512, 126.9882), vrp.Location(37.5133, 127.1001)]
#                    id    name  location demand ready     due        service (s)
visits = [vrp.Visit(f"C{i}", f"C{i}", i + 1, 2, 8 * 3600, 20 * 3600, 600) for i in range(3)]
vehicles = [vrp.Vehicle("V1", 6, 0, departure_time=7 * 3600)]
plan = vrp.VehicleRoutePlan("demo", locations, vehicles, visits)

config = vrp.SolverConfig()
config.spent_limit_seconds = 2
solver = vrp.Solver(config)

solver.solve(plan)                      # default constraints
print(plan.score, plan.routes())        # 0hard/0medium/-966soft {'V1': ['C0', 'C1', 'C2']}

# Pick and reweight constraints; the solver does not change.
solver.solve_with_constraints(plan, get_constraints([
    VehicleCapacityCost(),
    UnassignedVisitsCost(),
    TravelTimeCost(weight=10),
]))
print(plan.score)                       # 0hard/0medium/-9660soft
```

Every constraint takes the same knobs — `weight`, `level` (`"hard"`,
`"medium"`, `"soft"`), `exponent`, `offset` and `clip` — and may be picked
several times. `constraint_info()` on a use case lists its constraints and
their defaults. More in [`python/README.md`](python/README.md) and the
[notebooks](examples).

## Use cases

[`use-cases/README.md`](use-cases/README.md) lists all 22 use cases and
explains how to extend one — adding a move, changing the construction
heuristic, adding a hard or soft cost. Each use case's README has a short
usage example and the same guide with its own file names.

Scheduling: bed allocation, conference, employee shifts, flight crew, food
packaging, maintenance, meeting, project-job (RCPSP), school timetabling,
semiconductor fab, sports league, task assigning, tournament. Routing:
vehicle routing (CVRPTW), pickup & delivery (PDPTW), order picking. Assignment and packing:
facility location, bin packing, knapsack, quadratic assignment, max-cut,
assembly-line balancing.

```sh
cd use-cases/<slug>/python && uv sync --all-groups --all-extras && uv run pytest
```

## Benchmarks

[`benchmarks/README.md`](benchmarks/README.md) lists every comparison. In
short: every use case is compared with OR-Tools CP-SAT on the same instances
and rescored by the use case's own evaluator; vehicle routing is compared with
the OR-Tools Routing solver on CVRPLIB Set X, and school
timetabling with OR-Tools CP-SAT. Pickup & delivery,
project-job scheduling, bin packing, quadratic assignment and assembly-line
balancing also run on public instance sets (Li & Lim, RG30, Falkenauer,
QAPLIB, SALBP).

## Features

- **Solution shapes** (`SolutionTraits<Solution>`): assignment (each entity
  takes a value), sequence (ordered lists of elements) and chain (predecessor
  links from anchors, solved through a synchronized list view so every
  sequence move applies).
- **Derived state** via `DerivedState<Solution>`
  (`Solver::add_derived_state()`): values computed from the decisions, kept up
  to date around every write; `ScoreChecks::Full` also catches stale ones.
- **Scoring**: `Score` (int64 hard, double soft); constraints are written as an
  incremental `Objective` (assignment and list hooks) or summed from named
  components with `WeightedObjective`. Move evaluation stays incremental
  (O(change)).
- **Constraint plugins**: `DecomposedObjective` scores named unit and
  global components (`<Subject>Cost`). Each declares its default weight and
  level (`kDefaultWeight` / `kDefaultLevel`: hard, medium or soft); a caller
  re-states only five knobs per solve (`ConstraintAssembly`): `weight`,
  `level`, `exponent`, and `offset` / `clip`, which shift the shaping of a
  raw cost `x >= 0` as a threshold (`max(0, x - offset) ** exponent`) or as
  the literal `(x - offset) ** exponent`, per unit (each vehicle, slot, ...),
  not on the total. A constraint may be added several times
  (`add_instance()`), each instance with its own knobs, and the instances add
  up. Rewards (raw cost <= 0) never go hard, the literal formula with an
  offset never lands on a level that counts toward the hard score (so hard 0
  still means no violation), structural constraints keep their level, and a
  domain with a medium level declares once how it folds into the hard/soft
  score (`MediumFold`).
- **Score explanation**: `explain_score()` → `ScoreBreakdown`, a
  per-constraint split over the named components; `rescore()` recomputes a
  score from scratch.
- **Verification**: `ScoreChecks::Full` compares every incremental score with
  a full recompute and throws `ScoreMismatchError` on a difference.
- **Construction**: first fit, first fit decreasing, cheapest insertion for
  sequences.
- **Acceptors**: hill climbing, late acceptance, entity tabu, simulated
  annealing, great deluge, step counting hill climbing, record-to-record travel.
- **Limits** (`Limits`): wall time, stall time, steps, stall steps, target
  score; the first one reached ends the solve.
- **Moves**: change / swap (assignment); relocate / swap / 2-opt / Or-opt /
  2-opt\* tail swap / CROSS-exchange (sequence), mixed by weight in `MoveMix`.
- **Nearby selection**: `NeighborSettings::distance` biases moves toward
  k-nearest neighbours.
- **Ruin & recreate**: adaptive ALNS — random/related/worst/SISR/route ruin +
  regret-k (default k=3, nearby-restricted) recreate + polish, adaptive operator
  weights (roulette wheel by default; segmented roulette, alpha-UCB, uniform).
- **Solving**: `Solver<Solution>(settings).solve(solution, objective)`;
  `solve_in_background()` → `BackgroundSolve` (`status()`, `request_stop()`,
  `wait()`); `solve_multistart()` (N independent starts, best wins).
- **Custom operators** (`search/operators.hpp`): `add_move_operator` /
  `add_ruin_operator` / `set_acceptor_factory`.

Multithreaded *move* evaluation is deliberately excluded: it only pays when a
single score calculation is expensive (ms-scale); at this engine's ~200ns
moves, per-step thread synchronization would cost more than it saves — use
`solve_multistart()` to occupy cores instead. Also out of scope:
exhaustive/partitioned search and REST layers.

## Performance features

- **Nearby selection**: on spatial problems a distant relocate/swap is almost
  never an improvement, so moves are biased toward k-nearest neighbors. The
  same lists drive the LNS related ruin and the nearby-restricted repair.
- **LNS / ALNS**: escapes the local optima that acceptor-only search cannot
  leave — verified in the tests: hill climbing alone stays trapped on a
  conflicted instance, hill climbing + LNS reaches the global optimum.
- Measured on 100-city random TSP (5-seed sweep): nearby + LNS beats plain
  local search on every seed even with HALF the outer step budget
  (mean tour 2514 → 2409).

## Repository layout

The engine is dependency-free, header-only C++17 and contains **no domain
code** — problems consume it as a library. Worked use cases live in
[`use-cases/`](use-cases), runnable notebooks with visualizations in
[`examples/`](examples) and public benchmark harnesses in
[`benchmarks/`](benchmarks).

```
include/optimaru/     the engine (header-only)
test/                 engine tests
use-cases/<slug>/     22 problems built on the engine: C++ domain + solver
                      and a pybind11 package
examples/             one Jupyter notebook per use case, with visualizations
benchmarks/<slug>/    the same problems against OR-Tools (CP-SAT, or the
                      Routing solver for VRP);
                      competing solvers live only here
benchmarks/cpsat-common/  comparison runner; points external-solver at this checkout
```

## Engine layout

```
include/optimaru/
├── optimaru.hpp            umbrella include for consumers
├── solver.hpp              Solver: construction, then search; multi-start
├── background_solve.hpp    solve_in_background / BackgroundSolve
├── explain.hpp             explain_score / rescore
├── settings.hpp            Settings, Limits, AcceptanceSettings, MoveMix,
│                           NeighborSettings, RuinRecreateSettings, ...
├── worker_threads.hpp      thread start-up / shutdown for parallel pools
├── model/                  the solution as the engine sees it
│   ├── solution_traits.hpp SolutionTraits, Shape (Assignment | Sequence | Chain),
│   │                       pinning, chain link encoding
│   ├── derived_state.hpp   values derived from the decisions
│   ├── editor.hpp          every write, bracketed by the objective's hooks
│   ├── chain_view.hpp      the list view a chain solve runs on
│   ├── element_index.hpp   element -> (list, position)
│   ├── snapshot.hpp        whole and list-wise restore
│   └── neighborhoods.hpp   k-nearest-neighbour lists
├── score/                  scoring
│   ├── score.hpp           Score, ScoreChecks, ScoreMismatchError
│   ├── levels.hpp          Level (hard / medium / soft), MediumFold
│   ├── objective.hpp       Objective: the incremental scoring contract
│   ├── chain_objective.hpp, weighted_objective.hpp
│   ├── decomposed_objective.hpp   per-unit and global constraint plugins
│   ├── constraint_assembly.hpp    default constraint set: weight / level /
│   │                              exponent / offset / clip / drop by name,
│   │                              repeated instances, info() catalog
│   └── breakdown.hpp       ScoreBreakdown / ConstraintScore
├── construct/              the initial solution: first fit (decreasing),
│                           cheapest insertion
├── accept/                 Acceptor and one file per rule: hill climbing, late
│                           acceptance, entity tabu, simulated annealing, great
│                           deluge, step counting, record-to-record travel
├── select/                 adaptive operator selection: MoveOutcome, roulette
│                           wheel, segmented roulette, alpha-UCB, uniform
├── stop/                   when a solve ends (StopCondition) and the throttle
│                           long-running moves consult it through
├── search/                 the step loop (local_search) and the plugin points
│                           for custom operators (MoveOperator / RuinOperator)
├── moves/                  every move the step loop draws: the sequence moves
│                           (list_move) and their previews, descent, ejection
│                           chain, iterated local search, route elimination
├── ruin_recreate/          one destroy-and-repair unit: the ruin operators
│                           (random, related, worst, SISR, route), the greedy /
│                           regret-k recreate, and ruin_recreate_move, which
│                           applies them as an ordinary move (the stagnation
│                           kick uses the same parts)
├── python/                 pybind11 helpers: constraint specs ->
│                           ConstraintAssembly, constraint_info(),
│                           constraint_costs(), names, the level guard
└── testing/                preview-vs-applied-score harness for a use case's
                            objective tests
```

`optimaru::` is one flat namespace apart from the `optimaru::testing` and
`optimaru::python` helpers (and small `detail` / `chain` helper namespaces);
the directories organize the headers, the namespaces don't. Functions and
members are snake_case, types PascalCase.

## Tests

`test/engine_test.cpp` verifies every feature against brute force on two toy
domains (task assignment, TSP): six of the seven acceptors (all but
record-to-record travel), all five limits, FFD ordering, multi-start
(and its exception safety), `ScoreChecks::Full` catching a deliberately
corrupted objective, list moves under `ScoreChecks::Full`, adaptive operator-weight
convergence, LNS escaping a local optimum that traps hill climbing, and the
nearby+LNS quality win on 100-city random TSP. `test/constraint_level_test.cpp`
covers the constraint contract: levels and both medium folds, the knob guards
(rewards, structural levels, the literal offset kept off the hard score,
weight, exponent, offset and clip checks), the
catalog, exponent and offset / clip shaping, repeated instances,
incremental-vs-recompute and preview parity with a medium level and with
repeated and shifted components, and the score range (a saturating hard total,
out-of-range contributions).

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

## Building and releasing

The distribution is built from this repository by [`setup.py`](setup.py), which
reuses each use case's own `python/setup.py`.

**Releasing:** bump `version` in [`pyproject.toml`](pyproject.toml) in a PR and
merge it. On the push to `main`, if there is no `v<version>` tag yet,
[`wheels.yml`](.github/workflows/wheels.yml) builds the wheels and sdist, uploads
them to PyPI, then creates the `v<version>` tag and GitHub release. An unchanged
version neither builds nor publishes on `main`; PRs always build.

## License

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE) for attribution.

The engine began as a C++ port of parts of
[Timefold Solver](https://github.com/TimefoldAI/timefold-solver) and has since
been restructured with its own layout, names and API; portions still derive
from it. Several use cases translate examples from
[Timefold Quickstarts](https://github.com/TimefoldAI/timefold-quickstarts).
Both are Apache-2.0; [NOTICE](NOTICE) lists exactly which parts derive from
which. When you redistribute optimaru or a derivative of it, include both
[LICENSE](LICENSE) and [NOTICE](NOTICE). optimaru is not affiliated with or
endorsed by Timefold BV.
