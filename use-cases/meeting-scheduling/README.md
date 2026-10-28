# Meeting Scheduling

Domain model, constraints and API details: [`python/`](python/README.md).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/meeting-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import meeting_scheduling as ms

schedule = ms.demo_data()
result = ms.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(result.score.hard, result.score.soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`meeting_scheduling.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/meeting-scheduling/cpp -B build/meeting-scheduling && cmake --build build/meeting-scheduling -j
ctest --test-dir build/meeting-scheduling
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/ms/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/ms/constraints/` (currently `assign_larger_rooms_first`, `avoid_overtime`, `do_meetings_as_soon_as_possible`, `one_break_between_consecutive_meetings`, `overlapping_meetings`, `preferred_attendance_conflict`, `required_and_preferred_attendance_conflict`, `required_attendance_conflict`, `required_room_capacity`, `room_conflict`, `room_stability`, `start_and_end_on_same_day`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/ms/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `ms_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/meeting_scheduling/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/meeting-scheduling/cpsat`](../../benchmarks/meeting-scheduling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/meeting-scheduling`](../../benchmarks/meeting-scheduling) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.
