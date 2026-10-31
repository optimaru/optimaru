# Task Assigning

Domain model, constraints and API details: [`python/`](python/README.md).

## Usage

```sh
cd use-cases/task-assigning/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import task_assigning as ta
from task_assigning.demo import generate_demo_data

solution = generate_demo_data()
result = ta.solve(solution, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_soft0, result.score_soft1, result.score_soft2)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`task_assigning.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/task-assigning/cpp -B build/task-assigning && cmake --build build/task-assigning -j
ctest --test-dir build/task-assigning
```

## Starting from an existing solution

```python
problem = generate_demo_data()
solved = ta.solve(problem, time_limit_seconds=2.0, random_seed=42)
seeded = ta.with_initial_solution(problem, solved)  # or solved.model_dump(mode="json")
better = ta.solve(seeded, time_limit_seconds=2.0, random_seed=42)
```

`with_initial_solution` returns a new problem whose employees carry the solution's `tasks`, replacing the ones the problem had. The solve keeps those lists as its starting point and construction places only the tasks in no list, so a partial solution works. With `time_limit_seconds=0` a complete solution comes back as it went in. An unknown id or a task assigned twice raises `ValueError`.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **LIST**; the domain solver that configures the
engine is `cpp/include/ta/solver.hpp` + `cpp/src/solver.cpp`.

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
`cpp/include/ta/constraints/` (currently `critical_priority_task_end_time`, `major_priority_task_end_time`, `minimize_makespan`, `minimize_unassigned_tasks`, `minor_priority_task_end_time`, `no_missing_skills`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/ta/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `ta_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/task_assigning/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/task-assigning/cpsat`](../../benchmarks/task-assigning/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/task-assigning`](../../benchmarks/task-assigning) compares this use case with OR-Tools CP-SAT on the same instances.
