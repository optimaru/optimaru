# Food Packaging — C++ port

The `food-packaging` example model ported onto `include/optimaru/` (the header-only
engine). Files:

- `include/fp/domain.hpp` — domain structs, the HardMediumSoft↔HardSoft score
  mapping (`kMediumUnit = 10^10`), the LIST `SolutionDescriptor` specialization
  (including the position translation that hides the pinned prefix from the engine view)
- `include/fp/score_calculator.hpp` — the full-recompute reference implementation
  (`prepare` / `updateShadowVariables` / `calculateScore` / `analyze`)
- `include/fp/incremental_calculator.hpp` — incremental calculator on the before/after
  LIST hooks, O(changed line length) (re-derives the shadow chain per line)
- `include/fp/solver.hpp` — engine wrapper. `Line.operator` (Java's second planning
  variable) is optimized alternately by hill climbing between engine legs
- `src/demo_data.cpp` — reproduces the Java `DemoDataGenerator`
  (bit-identical `java.util.Random(37)` replica; Monday 00:00 == minute 0)
- `src/main.cpp` — runs the demo (default 10 s, non-zero exit when hard 0 is not reached)
- `test/fp_test.cpp` — per-constraint hand-computed cases, 2000 random LIST moves
  cross-checked incremental==full, FULL_ASSERT solve, pinning/pool/operator/
  demo checks

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/fp_test && ./build/fp_demo
```

## Semantic notes (vs Java)

- `minStartTime` and `priority` are data fields that **no constraint reads**, the same as
  in Java (a property of the original quickstart).
- The first job gets a zero-length cleaning window at the line start and starts producing
  immediately (exactly how the Java shadow supplier behaves).
- Job pairs on the same line also share an operator, but their cleaning windows are
  sequential and never overlap — the full calculator iterates the pairs as they are and
  the incremental calculator keeps only cross-line pairs (same meaning).
- Lines without an operator (Java's `operator == null`) are grouped together
  (the same as the null matching of Java's `Joiners.equal`).
- caveat: `Line.operator` is not co-searched per move as in the reference model but reassigned
  between engine legs. With operators >= lines the assignment is injective and the
  conflict constraint is always 0, so results are exactly equivalent (the demo is this
  case); with operators < lines it is an alternating heuristic.
- Pinned jobs must be a **prefix** of their line (validated by `prepare`).
  The quickstart's continuous planning pins jobs "before now", so that is always a
  prefix.
