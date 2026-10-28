# Order picking (C++)

Timefold quickstart [order-picking](https://github.com/TimefoldAI/timefold-quickstarts)
Port: only the domain and constraints are implemented, on top of the generic engine (`../../../include`, header-only).

- `include/op/domain.hpp` — warehouse layout (5 shelf columns x 3 rows, left/right sides, rows 0-10),
  `calculateDistance`, domain structs, the LIST-kind `SolutionDescriptor` specialization
  (the trolley's `pickTaskIndices` is the `@PlanningListVariable`).
- `include/op/score_calculator.hpp` — full recompute + shadow variable
  (`trolleyIndex`/`previousPickTask`/`nextPickTask`) recalculation + per-constraint breakdown.
- `include/op/incremental_calculator.hpp` — before/afterListChanged hooks for
  per-trolley O(list) incremental scoring (checked against full recompute under FULL_ASSERT).
- `include/op/solver.hpp` — engine wrapper (LIST move mix + ALNS + warehouse-distance nearby).
- `src/demo_data.cpp` — reproduces the Java `DemoDataGenerator` (bit-identical
  `java.util.Random(37)` replay: 37-product catalog, 8 orders, 5 trolleys x 4 buckets x 48000 cm3).

## Build / run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/op_test   # per-constraint hand checks + incremental==full + FULL_ASSERT solve
./build/op_demo   # 10 s solve of the Java demo dataset, reaches feasible (hard 0)
```
