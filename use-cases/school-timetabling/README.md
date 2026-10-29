# School Timetabling

Domain model, constraints and API details: [`python/`](python/README.md).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/school-timetabling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import json

problem = json.load(open("problem.json"))   # canonical TimetableProblem, see API_SPEC.md
import st_timetabling as st

timetable = st.solve(problem, time_limit_seconds=5.0, random_seed=42)  # canonical Timetable dict
print(timetable["status"], len(timetable["lessons"]))
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`st_timetabling.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/school-timetabling/cpp -B build/school-timetabling && cmake --build build/school-timetabling -j
ctest --test-dir build/school-timetabling
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/st/canonical_solver.hpp` + `cpp/src/canonical_solver.cpp` (the default canonical model; `solver.hpp` is the six-constraint quickstart model).

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). `SmartClassSwapMove`, `ReassignMove` and `UnassignMove` live in `cpp/include/st/moves/` and are registered in `cpp/src/canonical_solver.cpp` — add a new move as another file there. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/canonical_solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** `entityDifficulty` is already set in `cpp/src/canonical_solver.cpp`. Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/st/constraints/` (currently `class_gap`, `global/block_adjacency`, `global/course_daily_limit`, `global/unassigned_lesson`, `slot/avoided_period`, `slot/class_conflict`, `slot/non_preferred_period`, `slot/room_capacity`, `slot/slot_not_allowed`, `slot/teacher_conflict`, `teacher/teacher_consecutive_limit`, `teacher/teacher_free_day`, `teacher/teacher_gap`, `teacher/teacher_load_limit`, `teacher/teacher_workday`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/st/canonical_constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `canonical_score_calculator` / `canonical_incremental_calculator` — `canonical_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/st_timetabling/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/school-timetabling/cpsat`](../../benchmarks/school-timetabling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/school-timetabling`](../../benchmarks/school-timetabling) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.
