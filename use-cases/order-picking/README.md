# Order Picking

Domain model, constraints and API details: [`python/`](python/README.md) and [`cpp/`](cpp/README.md).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/order-picking/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import order_picking as op

solution = op.build_demo_data()
solution = op.solve(solution, time_limit_seconds=5.0, random_seed=0)
print(solution.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`order_picking.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/order-picking/cpp -B build/order-picking && cmake --build build/order-picking -j
ctest --test-dir build/order-picking
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **LIST**; the domain solver that configures the
engine is `cpp/include/op/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). `OrderConsolidateMove` (pulls an order's items onto one trolley) is defined in `cpp/src/solver.cpp` and registered there when `orderConsolidateMoves` is on — use it as the template. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Elements are placed by cheapest insertion in index order. Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/op/constraints/` (currently `minimize_distance_from_last_pick_task_to_path_origin`, `minimize_distance_from_previous_pick_task`, `minimize_order_split_by_trolley`, `required_number_of_buckets`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/op/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `op_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/order_picking/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/order-picking/cpsat`](../../benchmarks/order-picking/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/order-picking`](../../benchmarks/order-picking) compares this use case with OR-Tools CP-SAT on the same instances.
