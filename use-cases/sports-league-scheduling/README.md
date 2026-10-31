# Sports League Scheduling (C++ / Python)

The `sports-league-scheduling` example model (Apache-2.0; see the repository NOTICE)
ported onto the generic C++ engine in `include/optimaru/`. Assigns a round (`Round`) to
every match (`Match`) of the league (one BASIC variable: `Match::round`).

- `cpp/` — domain (`sls::LeagueSchedule`), full-recompute scorer, O(delta)
  incremental scorer, a thin solver wrapper (LA + ALNS), demo (`sls_demo`),
  tests (`sls_test`).
- `python/` — pybind11 package `sports_league` (with the pydantic mirror, demo generator
  and type stubs). Details in `python/README.md`.

## Build / verify

```sh
cd cpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/sls_test      # constraint hand checks + incremental==full over 3000 moves + FULL_ASSERT solve
./build/sls_demo 10   # reproduces the Java demo (14 teams/32 rounds/182 matches), 10 s solve, checks 0hard

cd ../python && uv venv && uv pip install -e . pytest pydantic && uv run pytest -q
```

## Score mapping

The Java original already uses `HardSoftScore`, so it maps identically onto the engine's
`HardSoftScore(int64 hard, double soft)` (soft stored as an integer value in the double).
The 10 constraints have the same semantics as Java `SportsLeagueSchedulingConstraintProvider`
— for the list see `python/README.md` or the comments in `cpp/include/sls/
score_calculator.hpp`.

## Usage

```sh
cd use-cases/sports-league-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import sports_league as sl

schedule = sl.demo_schedule()
sl.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(schedule.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`sports_league.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/sports-league-scheduling/cpp -B build/sports-league-scheduling && cmake --build build/sports-league-scheduling -j
ctest --test-dir build/sports-league-scheduling
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is `LeagueScheduleModel.from_core(schedule)` or that model as a JSON dict. `solve` takes the engine object, so convert the result with `to_core()`:

```python
from sports_league.models import LeagueScheduleModel

problem = LeagueScheduleModel.from_core(sl.demo_schedule())
solved = LeagueScheduleModel.from_core(schedule)                # `schedule` solved above
restart = sl.with_initial_solution(problem, solved).to_core()
sl.solve(restart, time_limit_seconds=5.0, random_seed=43)
print(restart.score)
```

Matches are matched by id. A match the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. `round` is a position in `rounds`, not `Round.index`, so a solution that lists different rounds, or the same ones in another order, raises `ValueError`. So do unknown ids and out-of-range values, before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/sls/solver.hpp` + `cpp/src/solver.cpp`.

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
`cpp/include/sls/constraints/` (currently `away_to_away_hop`, `away_to_end_hop`, `away_to_home_hop`, `classic_matches`, `consecutive_away_matches`, `consecutive_home_matches`, `home_to_away_hop`, `matches_on_same_day`, `repeat_match_on_next_day`, `start_to_away_hop`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/sls/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `sls_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/sports_league/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/sports-league-scheduling/cpsat`](../../benchmarks/sports-league-scheduling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/sports-league-scheduling`](../../benchmarks/sports-league-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.
