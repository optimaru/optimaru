# Food Packaging (C++ / Python)

A C++ engine port of the The `food-packaging` example model (Apache-2.0; see the repository NOTICE).
Assigns packaging jobs (Job) to production lines (Line) **in order** — every product change
inserts a cleanup time that depends on the previous product, and the cleaning windows of
lines run by the same operator must not overlap.

- `cpp/` — domain + constraints (full/incremental) + solver wrapper + demo/tests.
  Uses the engine in `include/` (header-only) as-is.
- `python/` — pybind11 bindings (`food_packaging._core`) and the pydantic v2 mirror.

## Model summary

| Java | Port |
|---|---|
| `Line.jobs` `@PlanningListVariable(allowsUnassignedValues=true)` | engine LIST variable. Unassigned is an unassigned pool exposed to the engine as one more list |
| `Line.operator` `@PlanningVariable` | reassigned by the solver wrapper with hill climbing between engine legs (alternating optimization) |
| `Job` shadow chain (`startCleaning/startProduction/end`) | derived from list positions, O(length) incremental recompute per line |
| `@PlanningPin` | structural pinning that hides the line's pinned prefix from the engine view |
| `HardMediumSoftScore` | `engine.soft = medium * 10^10 + soft` mapping (see the domain header comments) |

## Build/run

```sh
cd cpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/fp_test          # constraint hand checks + incremental==full + FULL_ASSERT + demo feasible
./build/fp_demo [sec]    # reproduces the Java demo (5 lines, 100 jobs), default 10 s

cd ../python && uv venv && uv pip install -e . pytest pydantic && uv run pytest -q
```

## Usage

```sh
cd use-cases/food-packaging/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import food_packaging as fp

solution = fp.generate_demo_data()
result = fp.solve(solution, time_limit_seconds=5.0, random_seed=42)
print(result.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`food_packaging.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/food-packaging/cpp -B build/food-packaging && cmake --build build/food-packaging -j
ctest --test-dir build/food-packaging
```

## Starting from an existing solution

```python
from food_packaging.models import PackagingScheduleModel

problem = PackagingScheduleModel.from_core(fp.generate_demo_data())
solved = PackagingScheduleModel.from_core(fp.solve(problem.to_core(), time_limit_seconds=2.0, random_seed=42))
seeded = fp.with_initial_solution(problem, solved)  # or solved.model_dump(mode="json")
better = fp.solve(seeded.to_core(), time_limit_seconds=2.0, random_seed=42)
```

`with_initial_solution` returns a new problem whose lines carry the solution's `job_ids` and `operator_id`, replacing the ones the problem had. The solve keeps those lists as its starting point and construction places only the jobs in no line, so a partial solution works. Pinned jobs stay as the problem has them: a line in the solution must start with its pinned jobs. An unknown id, a job given twice, a moved pinned job or two neighbouring jobs with no cleaning time between their products raise `ValueError`.

Two things differ from the other use cases, on the default solve only (without `constraints=`):

- Seeded operators can change. `improve_operator_assignment` runs after the search even with no time to search, and reassigns an operator whenever that strictly cuts the cleaning conflict.
- A time limit of 1 ms or less returns before construction, so left-out jobs stay unassigned. A complete solution comes back as it went in, but to have a partial one filled give it a longer limit.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **LIST**; the domain solver that configures the
engine is `cpp/include/fp/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Elements are placed by cheapest insertion in index order. Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/fp/constraints/` (currently `ideal_end_date_time`, `max_end_date_time`, `maximize_jobs_assigned`, `minimize_makespan`, `operator_cleaning_conflict`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/fp/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `fp_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/food_packaging/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/food-packaging/cpsat`](../../benchmarks/food-packaging/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/food-packaging`](../../benchmarks/food-packaging) compares this use case with OR-Tools CP-SAT on the same instances.
