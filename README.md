# optimaru

A dependency-free, header-only C++17 metaheuristic solver engine for
real-world combinatorial optimization — scheduling, routing and assignment.

The engine contains **no domain code** — problems consume it as a library.
Worked use cases live in [`use-cases/`](use-cases) and public benchmark
harnesses in [`benchmarks/`](benchmarks).

optimaru comes in a free edition and a paid edition; this repository is the
free, open-source edition.

## Install (Python)

```bash
pip install optimaru
```

One package with every use case as a subpackage — `optimaru.vehicle_routing`,
`optimaru.school_timetabling`, … — compiled for Linux, macOS and Windows
(Python 3.14+). See [`python/README.md`](python/README.md) for an example.
The distribution is built from this repository by [`setup.py`](setup.py), which
reuses each use case's own `python/setup.py`; releases are published by
[`wheels.yml`](.github/workflows/wheels.yml).

## Repository layout

```
include/optimaru/     the engine (header-only, one header per class)
test/                 engine tests
use-cases/<slug>/     21 problems built on the engine: C++ domain + solver
                      and a pybind11 package (or a small pure-Python model)
benchmarks/<slug>/    the same problems against OR-Tools (CP-SAT, or the
                      Routing solver for VRP) and, where available, Timefold;
                      competing solvers live only here
benchmarks/cpsat-common/  shared CP-SAT constraint-plugin contract
```

## Engine layout

```
include/optimaru/
├── optimaru.hpp                       umbrella include for consumers
├── api/
│   ├── domain/solution_descriptor.hpp             planning variable description
│   │                                              (VariableKind::BASIC | LIST | CHAINED)
│   ├── domain/variable/variable_listener.hpp      shadow variables
│   ├── score/buildin/hard_soft_score.hpp          HardSoftScore
│   ├── score/calculator/incremental_score_calculator.hpp
│   ├── score/calculator/chained_incremental_score_calculator.hpp
│   ├── score/calculator/composite_incremental_score_calculator.hpp
│   │                                              cost components, weighted sum
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
│   ├── solver/termination/termination.hpp         Termination hierarchy +
│   │                                              SolverTermination (OR-composite)
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
│   ├── heuristic/selector/common/nearby/nearby_neighborhoods.hpp
│   └── heuristic/selector/move/generic/ruin_recreate_selector.hpp
│                                                  LnsOutcome + RouletteWheelSelector
├── preview/api/move/operators.hpp                 custom MoveOperator / RuinOperator
│                                                  registration (experimental API)
└── testing/preview_parity.hpp                     move-preview vs applied-score harness
                                                   for a use case's calculator tests
```

`optimaru::` is one flat namespace; the directories organize the headers,
the namespaces don't.

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
- **Score analysis**: `SolutionManager::analyze()` → `ScoreAnalysis` /
  `ConstraintAnalysis`, a per-constraint breakdown over the named components.
- **Verification**: `ScoreDirector` with `EnvironmentMode::FULL_ASSERT`;
  score corruption throws `ScoreCorruptionError`.
- **Construction heuristics**: First Fit, First Fit Decreasing, cheapest
  insertion for list variables.
- **Acceptors**: Hill Climbing, Late Acceptance, Entity Tabu, Simulated
  Annealing, Great Deluge, Step Counting Hill Climbing.
- **Terminations**: spent / unimproved-spent / step count /
  unimproved-step-count / best score, OR-composed.
- **Moves**: change/swap (BASIC); relocate / swap / 2-opt / Or-opt / 2-opt\*
  tail swap (LIST), mixed by weight in `MoveSelectorConfig`.
- **Nearby selection**: `NearbySelectionConfig::distanceMeter` biases moves
  toward k-nearest neighbors.
- **Ruin & recreate**: adaptive ALNS — random/related/worst ruin + greedy
  (nearby-restricted) recreate + polish, roulette-wheel operator weights.
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

## Using it

Header-only: add `include/` to the include path, or from CMake:

```cmake
add_subdirectory(optimaru)   # or FetchContent
target_link_libraries(my_app PRIVATE optimaru::optimaru)
```

```cpp
#include "optimaru/optimaru.hpp"

// 1. Describe the planning variable:
template <>
struct optimaru::SolutionDescriptor<MySolution> {
    static constexpr optimaru::VariableKind kind = optimaru::VariableKind::BASIC;
    static int entityCount(const MySolution&);
    static int valueCount(const MySolution&);
    static int getVariable(const MySolution&, int entity);   // -1 == unassigned
    static void setVariable(MySolution&, int entity, int value);
};
// (LIST kind instead: listCount/elementCount/listSize/getElement/
//  insertElement/removeElement/reverseSegment)

// 2. Implement the constraints incrementally (or compose cost components
//    with CompositeIncrementalScoreCalculator):
class MyCalculator final : public optimaru::IncrementalScoreCalculator<MySolution> { ... };

// 3. Configure and solve:
optimaru::SolverConfig config;
config.termination.spentLimit = std::chrono::seconds(30);
config.localSearch.acceptor.acceptorType = optimaru::AcceptorType::LATE_ACCEPTANCE;
config.localSearch.nearby.distanceMeter = [&](int a, int b) { return dist(a, b); };
config.localSearch.ruinRecreate.enabled = true;   // ALNS kicks on stagnation

auto solver = optimaru::SolverFactory<MySolution>::create(config).buildSolver();
solver.addMoveOperator(std::make_unique<MyOrOptMove>());   // optional customs
MyCalculator calculator;
solver.solve(solution, calculator);
```

For a complete, production-shaped example see
[`use-cases/vehicle-routing`](use-cases/vehicle-routing).

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

## Use cases

[`use-cases/README.md`](use-cases/README.md) lists all 21 use cases and
explains how to extend one — adding a move, changing the construction
heuristic, adding a hard or soft cost. Each use case's README has a short
usage example and the same guide with its own file names.

Scheduling: bed allocation, conference, flight crew, food packaging,
maintenance, meeting, project-job (RCPSP), school timetabling, semiconductor
fab, sports league, task assigning, tournament. Routing: vehicle routing
(CVRPTW), pickup & delivery (PDPTW), order picking. Assignment and packing:
facility location, bin packing, knapsack, quadratic assignment, max-cut,
assembly-line balancing.

```sh
# C++
cmake -S use-cases/<slug>/cpp -B build/<slug> && cmake --build build/<slug> -j
ctest --test-dir build/<slug>

# Python (uv)
cd use-cases/<slug>/python && uv sync --all-groups --all-extras && uv run pytest
```

## Benchmarks

[`benchmarks/README.md`](benchmarks/README.md) lists every comparison. In
short: every use case is compared with OR-Tools CP-SAT on the same instances
and rescored by the use case's own evaluator; vehicle routing is compared with
the OR-Tools Routing solver and Timefold Solver on CVRPLIB Set X, and school
timetabling three ways (engine / CP-SAT / Timefold). Pickup & delivery,
project-job scheduling, bin packing, quadratic assignment and assembly-line
balancing also run on public instance sets (Li & Lim, Patterson, Hexaly BPP,
QAPLIB, SALBP).

## Tests

`test/engine_test.cpp` verifies every feature against brute force on two toy
domains (BASIC task assignment, LIST TSP): all six acceptors, all four
terminations, FFD ordering, multi-start, FULL_ASSERT catching a deliberately
corrupted calculator, list moves under FULL_ASSERT, adaptive operator-weight
convergence, LNS escaping a local optimum that traps hill climbing, and the
nearby+LNS quality win on 100-city random TSP.

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

## License

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE) for attribution.
