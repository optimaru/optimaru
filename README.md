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

- **Planning variables**: BASIC (entity → value), LIST (ordered lists of
  elements) and CHAINED (previous pointers/anchors, solved through a
  synchronized list view so the full LIST move repertoire applies).
- **Shadow variables** via `VariableListener<Solution>`
  (`Solver::addVariableListener()`); FULL_ASSERT also catches stale shadows.
- **Scoring**: `HardSoftScore` (int64 hard, double soft); constraints are
  written as an `IncrementalScoreCalculator` (list hooks included) or composed
  from named cost components with `CompositeIncrementalScoreCalculator`.
  Move evaluation stays incremental (O(delta)).
- **Constraint plugins**: `DecomposedScoreCalculator` scores named unit and
  global components (`<Subject>Cost`). Each declares its default weight and
  level (`kDefaultWeight` / `kDefaultLevel`: hard, medium or soft); a caller
  re-states only five knobs per solve (`ConstraintAssembly`): `weight`,
  `level`, `exponent`, and `offset` / `clip`, which shift the shaping of a
  raw cost `x >= 0` as a threshold (`max(0, x - offset) ** exponent`) or as
  the literal `(x - offset) ** exponent`, per unit (each vehicle, slot, ...),
  not on the total. A constraint may be added several times
  (`addInstance()`), each instance with its own knobs, and the instances add
  up. Rewards (raw cost <= 0) never go hard, the literal formula with an
  offset never lands on a level that counts toward the hard score (so hard 0
  still means no violation), structural constraints keep their level, and a
  domain with a medium level declares once how it folds into the hard/soft
  score (`MediumFold`).
- **Score analysis**: `SolutionManager::analyze()` → `ScoreAnalysis` /
  `ConstraintAnalysis`, a per-constraint breakdown over the named components.
- **Verification**: `ScoreDirector` with `EnvironmentMode::FULL_ASSERT`;
  score corruption throws `ScoreCorruptionError`.
- **Construction heuristics**: First Fit, First Fit Decreasing, cheapest
  insertion for list variables.
- **Acceptors**: Hill Climbing, Late Acceptance, Entity Tabu, Simulated
  Annealing, Great Deluge, Step Counting Hill Climbing, Record-to-Record Travel.
- **Terminations**: spent / unimproved-spent / step count /
  unimproved-step-count / best score, OR-composed.
- **Moves**: change/swap (BASIC); relocate / swap / 2-opt / Or-opt / 2-opt\*
  tail swap / CROSS-exchange (LIST), mixed by weight in `MoveSelectorConfig`.
- **Nearby selection**: `NearbySelectionConfig::distanceMeter` biases moves
  toward k-nearest neighbors.
- **Ruin & recreate**: adaptive ALNS — random/related/worst/SISR/route ruin +
  regret-k (default k=3, nearby-restricted) recreate + polish, adaptive operator
  weights (roulette wheel by default; segmented roulette, alpha-UCB, random).
- **Solving**: `SolverFactory` / `Solver` / `SolverConfig`; async background
  solving with `SolverManager` / `SolverJob` / `SolverStatus` and
  `terminateEarly()`; `solveMultiStart()` (N independent starts, best wins).
- **Custom moves** (`preview/api/move`): `addMoveOperator` /
  `addRuinOperator` / `setAcceptorFactory`.

Multithreaded *move* evaluation is deliberately excluded: it only pays when a
single score calculation is expensive (ms-scale); at this engine's ~200ns
moves, per-step thread synchronization would cost more than it saves — use
`solveMultiStart()` to occupy cores instead. Also out of scope:
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
include/optimaru/     the engine (header-only, one header per class)
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

The engine is a C++ reimplementation, with modifications, of the
[Timefold Solver](https://github.com/TimefoldAI/timefold-solver) core
(Apache-2.0); see [NOTICE](NOTICE).

```
include/optimaru/
├── optimaru.hpp                       umbrella include for consumers
├── api/
│   ├── domain/solution_descriptor.hpp             planning variable description
│   │                                              (VariableKind::BASIC | LIST | CHAINED)
│   ├── domain/variable/variable_listener.hpp      shadow variables
│   ├── score/buildin/hard_soft_score.hpp          HardSoftScore
│   ├── score/level.hpp                            Level (hard / medium / soft),
│   │                                              MediumFold
│   ├── score/calculator/incremental_score_calculator.hpp
│   ├── score/calculator/chained_incremental_score_calculator.hpp
│   ├── score/calculator/composite_incremental_score_calculator.hpp
│   │                                              cost components, weighted sum
│   ├── score/calculator/decomposed_score_calculator.hpp
│   │                                              per-unit constraint plugins
│   ├── score/calculator/constraint_assembly.hpp   default constraint set: weight / level /
│   │                                              exponent / offset / clip / drop by name,
│   │                                              repeated instances, info() catalog
│   ├── score/director/score_director.hpp          + EnvironmentMode::FULL_ASSERT
│   ├── score/analysis/score_analysis.hpp          ScoreAnalysis / ConstraintAnalysis
│   └── solver/{solver.hpp, solver_factory.hpp,
│               solution_manager.hpp, solver_manager.hpp}
├── config/                            SolverConfig as plain structs
│   ├── solver/solver_config.hpp
│   ├── solver/termination/termination_config.hpp
│   ├── constructionheuristic/construction_heuristic_config.hpp
│   ├── localsearch/local_search_config.hpp        + RuinRecreateConfig (ALNS)
│   ├── localsearch/decider/acceptor/local_search_acceptor_config.hpp
│   ├── heuristic/selector/move_selector_config.hpp  move mix weights (union selector)
│   └── heuristic/selector/common/nearby/nearby_selection_config.hpp
├── impl/
│   ├── solver/default_solver.hpp                  phase orchestration, multi-start
│   ├── solver/termination/                        termination_criterion.hpp (plugin point),
│   │                                              one file per criterion, solver_termination.hpp
│   │                                              (OR-composite); termination.hpp = umbrella
│   ├── constructionheuristic/default_construction_heuristic_phase.hpp
│   ├── localsearch/default_local_search_phase.hpp step loop, sampled moves, LNS
│   ├── localsearch/list_move.hpp                  the LIST moves every strategy
│   │                                              previews / applies / undoes
│   ├── localsearch/element_index.hpp              element -> (list, position)
│   ├── localsearch/solution_snapshot.hpp          whole and list-wise restore
│   ├── localsearch/strategy/                      Descent, EjectionChain,
│   │                                              IteratedLocalSearch, ...
│   ├── localsearch/decider/acceptor/              one class per subdirectory:
│   │   ├── acceptor.hpp                           hillclimbing/ lateacceptance/
│   │   │                                          tabu/ simulatedannealing/
│   │   │                                          greatdeluge/ stepcountinghillclimbing/
│   │   │                                          recordtorecordtravel/
│   ├── heuristic/selector/common/nearby/nearby_neighborhoods.hpp
│   └── heuristic/selector/move/generic/           ruin/ one ruin operator per file;
│                                                  select/ LnsOutcome + roulette / segmented /
│                                                  alpha-UCB / random schemes;
│                                                  ruin_recreate_selector.hpp = umbrella + factory
├── preview/api/move/operators.hpp                 custom MoveOperator / RuinOperator
│                                                  registration (experimental API)
├── python/constraint_binding.hpp                  pybind11 helper: Python constraint specs
│                                                  -> ConstraintAssembly, constraint_info(),
│                                                  constraint_costs()
├── python/{names,level_guard}.hpp                 Python names, the level guard
└── testing/preview_parity.hpp                     move-preview vs applied-score harness
                                                   for a use case's calculator tests
```

`optimaru::` is one flat namespace apart from the `optimaru::testing` and
`optimaru::python` helpers (and small `detail` / `chained` helper
namespaces); the directories organize the headers, the namespaces don't.

## Tests

`test/engine_test.cpp` verifies every feature against brute force on two toy
domains (BASIC task assignment, LIST TSP): six of the seven acceptors (all but
Record-to-Record Travel), all five terminations, FFD ordering, multi-start
(and its exception safety), FULL_ASSERT catching a deliberately
corrupted calculator, list moves under FULL_ASSERT, adaptive operator-weight
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

The engine and several of the use cases are derived from Apache-2.0 works —
[Timefold Solver](https://github.com/TimefoldAI/timefold-solver) and
[Timefold Quickstarts](https://github.com/TimefoldAI/timefold-quickstarts) —
translated into C++ and Python and modified; [NOTICE](NOTICE) lists exactly
which parts. When you redistribute optimaru or a derivative of it, include
both [LICENSE](LICENSE) and [NOTICE](NOTICE). optimaru is not affiliated with
or endorsed by Timefold BV.
