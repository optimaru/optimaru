# Order picking (C++)

The `order-picking` example model (Apache-2.0; see the repository NOTICE)
Port: only the domain and constraints are implemented, on top of the generic engine (`../../../include`, header-only).

- `include/op/domain.hpp` — warehouse layout (5 shelf columns x 3 rows, left/right sides, rows 0-10),
  `calculateDistance`, domain structs, the LIST-kind `SolutionTraits` specialization
  (the trolley's `pickTaskIndices` is the `@PlanningListVariable`).
- `include/op/score_calculator.hpp` — full recompute + shadow variable
  (`trolleyIndex`/`previousPickTask`/`nextPickTask`) recalculation + per-constraint totals
  (`ConstraintTotals`, members named after the registry names, at the components'
  `kDefaultWeight`).
- `include/op/constraints/` — one component per constraint: `trolley_capacity_cost.hpp`
  (`TrolleyCapacityCost`, hard 1), `distance_from_previous_pick_task_cost.hpp`
  (`DistanceFromPreviousPickTaskCost`, soft 1), `distance_from_last_pick_task_to_path_origin_cost.hpp`
  (`DistanceFromLastPickTaskToPathOriginCost`, soft 1), `order_split_by_trolley_cost.hpp`
  (`OrderSplitByTrolleyCost`, soft 1000) (+ `common.hpp`); `include/op/constraints.hpp`
  assembles the default set (`defaultConstraints()`, also the `constraint_info()` catalog).
- `include/op/incremental_calculator.hpp` — before/after_list_edit hooks for
  per-trolley O(list) incremental scoring (checked against full recompute under ScoreChecks::Full).
- `include/op/solver.hpp` — engine wrapper (LIST move mix + ALNS + warehouse-distance nearby).
- `src/demo_data.cpp` — reproduces the Java `DemoDataGenerator` (bit-identical
  `java.util.Random(37)` replay: 37-product catalog, 8 orders, 5 trolleys x 4 buckets x 48000 cm3).

## Build / run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/op_test         # per-constraint hand checks + incremental==full + ScoreChecks::Full solve
./build/op_plugin_test  # plugin assembly == monolith, raw costs by hand, catalog, overrides
./build/op_demo         # 10 s solve of the Java demo dataset, reaches feasible (hard 0)
```
