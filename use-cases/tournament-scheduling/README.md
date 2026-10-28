# Tournament Scheduling

Domain model, constraints and API details: [`python/`](python/README.md).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/tournament-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import tournament_scheduling as ts
from tournament_scheduling.models import demo_data

schedule = demo_data()
result = ts.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`tournament_scheduling.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/tournament-scheduling/cpp -B build/tournament-scheduling && cmake --build build/tournament-scheduling -j
ctest --test-dir build/tournament-scheduling
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/ts/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). `TeamCountMove` is defined and registered in `cpp/src/solver.cpp` — use it as the template. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/ts/constraints/` (currently `evenly_confrontation_count`, `fair_assignment_count_per_team`, `one_assignment_per_date_per_team`, `unavailability_penalty`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/ts/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `ts_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/tournament_scheduling/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/tournament-scheduling/cpsat`](../../benchmarks/tournament-scheduling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/tournament-scheduling`](../../benchmarks/tournament-scheduling) compares this use case with OR-Tools CP-SAT and Timefold Solver 2.6.0 on the same instances. Every solution is rescored by the Timefold quickstart's `ConstraintProvider`, so all three are judged by the same rules.

Latest run (30 s per solver, 3 instances): best score by the engine on 3, CP-SAT on 1, Timefold on 0 (ties count for each). Full table: [benchmarks/tournament-scheduling/README.md](../../benchmarks/tournament-scheduling/README.md#results).
