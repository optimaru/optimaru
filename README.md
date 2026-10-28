# optimaru

A dependency-free, header-only C++17 metaheuristic solver engine for
real-world combinatorial optimization — scheduling, routing and assignment.

The architecture and API are modelled on
[Timefold Solver](https://github.com/TimefoldAI/timefold-solver) (Apache-2.0):
the directory layout mirrors the upstream Java packages
(`ai.timefold.solver.core.{api,config,impl,preview}`), with the same class
names and one header per class. The engine contains **no domain code** —
problems consume it as a library. optimaru is an independent project and is
not affiliated with or endorsed by Timefold.

optimaru comes in a free edition and a paid edition; this repository is the
free, open-source edition.

## Layout (mirrors `ai/timefold/solver/core/`)

```
include/optimaru/
├── optimaru.hpp                       umbrella include for consumers
├── api/
│   ├── domain/solution_descriptor.hpp             @Planning* annotations analog
│   │                                              (VariableKind::BASIC | LIST)
│   ├── domain/variable/variable_listener.hpp      shadow variables
│   ├── score/calculator/chained_incremental_score_calculator.hpp
│   ├── score/buildin/hard_soft_score.hpp          HardSoftScore
│   ├── score/calculator/incremental_score_calculator.hpp
│   ├── score/calculator/composite_incremental_score_calculator.hpp
│   │                                              cost components, weighted sum
│   ├── score/director/score_director.hpp          + EnvironmentMode::FULL_ASSERT
│   ├── score/analysis/score_analysis.hpp           ScoreAnalysis / ConstraintAnalysis
│   └── solver/{solver.hpp, solver_factory.hpp,
│               solution_manager.hpp, solver_manager.hpp}
├── config/                            SolverConfig as plain structs (no XML)
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
│   ├── localsearch/decider/acceptor/              one class per subdir, as upstream:
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

Deviations from upstream, for a header-only C++ engine: `optimaru::` is one
flat namespace (directories mirror the packages, namespaces don't);
`Solver` is an alias of `DefaultSolver` instead of an interface; the
termination classes share one header; configs are structs, not XML.

## Mapping from Timefold

| Java (`ai.timefold.solver.core`) | C++ |
| --- | --- |
| `@PlanningSolution` / `@PlanningVariable` / `@PlanningListVariable` | `SolutionDescriptor<Solution>` specialization (BASIC / LIST) |
| `@ShadowVariable` / `VariableListener` | `VariableListener<Solution>` via `Solver::addVariableListener()`; FULL_ASSERT also catches stale shadows |
| chained `@PlanningVariable` (previous pointers, anchors) | `VariableKind::CHAINED` + `ChainedIncrementalScoreCalculator`; solved through a synchronized list view (`ChainedWorkingSolution`), full move repertoire applies |
| `HardSoftBigDecimalScore` | `HardSoftScore` (int64 hard, double soft) |
| `IncrementalScoreCalculator` | same name; list hooks added |
| composed/weighted constraints | `CompositeIncrementalScoreCalculator` (cost components) |
| `ScoreDirector`, `EnvironmentMode.FULL_ASSERT` | same names; corruption throws `ScoreCorruptionError` |
| `SolverFactory`, `Solver`, `SolverConfig` | same names |
| `SolverManager` / `SolverJob` / `SolverStatus` / `Solver.terminateEarly()` | same names (async background solve) |
| `SolutionManager.analyze()` → `ScoreAnalysis` / `ConstraintAnalysis` | same names; per-constraint breakdown via named `CompositeIncrementalScoreCalculator` components |
| acceptors: Hill Climbing, Late Acceptance, Entity Tabu, Simulated Annealing, Great Deluge, Step Counting Hill Climbing | same class names, upstream subdirectories |
| terminations: spent / unimproved-spent / step count / unimproved-step-count / best score, OR-composed | same class names |
| CH: First Fit, First Fit Decreasing; list construction | `DefaultConstructionHeuristicPhase` (+ cheapest insertion) |
| move selector composition (union selector with weights) | `MoveSelectorConfig` weights: change/swap (BASIC); relocate / swap / 2-opt / **Or-opt** / **2-opt\* tail swap** (LIST) |
| nearby selection (`NearbyDistanceMeter`) | `NearbySelectionConfig::distanceMeter` → k-NN move bias |
| ruin & recreate | adaptive ALNS: random/related/worst ruin + greedy (nearby-restricted) recreate + polish, roulette-wheel weights |
| multithreaded solving | `solveMultiStart()` (N independent starts, best wins) |
| custom moves | `preview/api/move`: `addMoveOperator` / `addRuinOperator` / `setAcceptorFactory` |

Not included: Constraint Streams (Bavet) — constraints are written as an
`IncrementalScoreCalculator` (also an official Timefold API), which keeps move
evaluation incremental (O(delta)) without a Bavet-style node network.
Multithreaded MOVE evaluation is deliberately excluded: it only pays when a
single score calculation is expensive (ms-scale); at this engine's ~200ns
moves, per-step thread synchronization would cost more than it saves — use
`solveMultiStart()` to occupy cores instead. Also out of scope:
exhaustive/partitioned search, benchmarker, REST layers.

## Using it

Header-only: add `include/` to the include path, or from CMake:

```cmake
add_subdirectory(optimaru)   # or FetchContent
target_link_libraries(my_app PRIVATE optimaru::optimaru)
```

```cpp
#include "optimaru/optimaru.hpp"

// 1. Describe the planning variable (the "annotations"):
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
