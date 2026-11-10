# Food Packaging — C++

The C++ lives in [`optimaru/food_packaging/cpp/`](../../../optimaru/food_packaging/cpp); the paths below are relative to it,
and its C++ tests are in [`tests/food_packaging/cpp/`](../../../tests/food_packaging/cpp).

The food-packaging use case implemented on `optimaru/cpp/optimaru/` (the header-only
engine). Files:

- `include/fp/domain.hpp` — domain structs, the mapping of the three score
  levels (hard / medium / soft) onto the engine's two (`kMediumUnit = 10^10`, declared once as `kMediumFold =
  MediumFold::intoSoft(10^10)` and registered with `MediumFoldOf<PackagingSchedule>`), the
  sequence-shape `SolutionTraits` specialization (including the position translation that hides
  the pinned prefix from the engine view)
- `include/fp/constraints/*_cost.hpp` — one plugin component per constraint
  (`MaxEndTimeCost`, `AfterIdealEndTimeCost`, `SquaredMakespanCost`,
  `OperatorCleaningConflictCost`, `UnassignedJobsCost`), each declaring its
  `kDefaultWeight` / `kDefaultLevel`; `include/fp/constraints.hpp` assembles the default set
- `include/fp/score_calculator.hpp` — the full-recompute reference implementation
  (`prepare` / `updateShadowVariables` / `score` / `analyze`, whose
  `ConstraintBreakdown` members carry the registry names; C++ only)
- `include/fp/incremental_calculator.hpp` — incremental calculator on the before/after
  list hooks, O(changed line length) (re-derives the job times per line)
- `include/fp/solver.hpp` — engine wrapper. `Line::operator_index` (the second decision
  variable) is optimized alternately by hill climbing between engine legs
- `src/demo_data.cpp` — the demo plant (5 lines, 100 jobs), drawn
  from a seeded `std::mt19937_64` (seed 37; Monday 00:00 == minute 0)
- `src/main.cpp` — runs the demo (default 10 s, non-zero exit when hard 0 is not reached)
- `tests/food_packaging/cpp/fp_test.cpp` — per-constraint hand-computed cases, 2000 random sequence moves
  cross-checked incremental==full, ScoreChecks::Full solve, pinning/pool/operator/
  demo checks
- `tests/food_packaging/cpp/fp_plugin_test.cpp` — the plugin assembly == the monolith, plugin incremental ==
  full, raw costs by hand (a positive and a zero case per constraint), the catalog
  (`info()` == the components' constants), the medium level (fold, `hard()` / `soft()` /
  weight), ScoreChecks::Full plugin solves and `on_new_best` reporting the same levels as the
  final `schedule.score`

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=food_packaging
cmake --build build -j4
./build/food_packaging/fp_test && ./build/food_packaging/fp_plugin_test && ./build/food_packaging/fp_demo
```

## Semantic notes

- `minStartTime` and `priority` are data fields that **no constraint reads**.
- The first job gets a zero-length cleaning window at the line start and starts producing
  immediately.
- Job pairs on the same line also share an operator, but their cleaning windows are
  sequential and never overlap — the full calculator iterates the pairs as they are and
  the incremental calculator keeps only cross-line pairs (same meaning).
- Lines without an operator (`operator_index == -1`) are grouped together, as if they
  shared one operator.
- caveat: `Line::operator_index` is not searched together with the job order, move by
  move, but reassigned between engine legs. With operators >= lines the assignment is
  injective and the conflict constraint is always 0, so nothing is lost (the demo is this
  case); with operators < lines it is an alternating heuristic.
- Pinned jobs must be a **prefix** of their line (validated by `prepare`).
  Pinning the jobs that have already started (those "before now") always gives a
  prefix.
