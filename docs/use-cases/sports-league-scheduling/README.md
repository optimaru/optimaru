# Sports league scheduling

Derived from the Timefold Quickstarts `sports-league-scheduling` example
(Apache-2.0; see the repository [NOTICE](../../../NOTICE)), with modifications,
on the generic C++ engine in `optimaru/cpp/optimaru/`. Assigns a round (`Round`) to
every match (`Match`) of the league (one assignment variable: `Match::round`).

- `optimaru/sports_league_scheduling/cpp/` — domain (`sls::LeagueSchedule`), full-recompute scorer, O(delta)
  incremental scorer, a thin solver wrapper (LA + ALNS), demo (`sls_demo`),
  tests (`sls_test`, `sls_plugin_test`).
- `optimaru/sports_league_scheduling/cpp/include/sls/constraints/` — one component per constraint (+ `common.hpp`):
  `team_conflict_cost.hpp`, `consecutive_home_matches_cost.hpp`,
  `consecutive_away_matches_cost.hpp`, `repeat_match_on_next_day_cost.hpp`,
  `start_to_away_hop_cost.hpp`, `home_to_away_hop_cost.hpp`,
  `away_to_away_hop_cost.hpp`, `away_to_home_hop_cost.hpp`,
  `away_to_end_hop_cost.hpp`, `workday_classic_matches_cost.hpp`;
  `optimaru/sports_league_scheduling/cpp/include/sls/constraints.hpp` assembles the default set (`defaultConstraints()`).
- `optimaru/sports_league_scheduling/` — pybind11 package `optimaru.sports_league_scheduling` (with the pydantic mirror, demo generator
  and type stubs). Details in [`python.md`](python.md).

## Usage

```sh
OPTIMARU_USE_CASES=sports_league_scheduling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/sports_league_scheduling
```

```python
from optimaru import sports_league_scheduling as sl

schedule = sl.demo_schedule()
sl.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(schedule.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`optimaru.sports_league_scheduling.SolverConfig` or `models.SolveConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=sports_league_scheduling && cmake --build build -j
ctest --test-dir build
```

## Constraints

This use case scores hard and soft only, so it maps identically onto the engine's
`Score(int64 hard, double soft)` (soft stored as an integer value in the double).
The 10 constraints
(more detail in [`python.md`](python.md) and the comments in `optimaru/sports_league_scheduling/cpp/include/sls/
score_calculator.hpp`):

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `start_to_away_hop_cost` | `StartToAwayHopCost` | soft | 1 | per round position: km the away teams travel to its matches when round index - 1 does not exist (first round, or first after a gap) |
| `away_to_end_hop_cost` | `AwayToEndHopCost` | soft | 1 | per round position: km the away teams travel home after its matches when round index + 1 does not exist (last round, or last before a gap) |
| `workday_classic_matches_cost` | `WorkdayClassicMatchesCost` | soft | 1000 | per round position: the classic matches (derbies) in it when it is neither a weekend nor a holiday |
| `team_conflict_cost` | `TeamConflictCost` | hard | 1 | total: match pairs on one round index sharing a team (a pair sharing both teams counts once) |
| `consecutive_home_matches_cost` | `ConsecutiveHomeMatchesCost` | hard | 1 | total: the full length of every run of 4+ consecutive home round indices, summed over teams |
| `consecutive_away_matches_cost` | `ConsecutiveAwayMatchesCost` | hard | 1 | total: the same for away runs |
| `repeat_match_on_next_day_cost` | `RepeatMatchOnNextDayCost` | hard | 1 | total: matches whose return fixture is on the next round index (once per match) |
| `home_to_away_hop_cost` | `HomeToAwayHopCost` | soft | 1 | total: km from a team's home ground to the next host after a home round |
| `away_to_away_hop_cost` | `AwayToAwayHopCost` | soft | 1 | total: km between two consecutive hosts of a team playing away twice |
| `away_to_home_hop_cost` | `AwayToHomeHopCost` | soft | 1 | total: km back home after an away round (the earlier match's distance) |

The order is `list_constraints()`: the three unit constraints (one cost per round
position) first, then the seven global constraints (one total each). Every constraint is
flippable (`level="hard"` / `level="soft"`, or `Level.HARD` / `Level.SOFT`;
`level="medium"` raises `ValueError`, the domain has no medium level), none is a reward
and all ten are scored by default (`default_enabled`); `optimaru.sports_league_scheduling.constraint_info()`
reports exactly this table from the C++ components (`kDefaultLevel` / `kDefaultWeight`).
The consecutive rules charge the whole run length (a run of 4 costs 4).

`constraint_costs()` is the per-constraint surface. The CP-SAT reference reads its
defaults from `constraint_info()`, so it cannot disagree with the engine on a default.

Every constraint takes the knobs `weight`, `level`, `exponent`, `offset` and `clip`
(`offset` and `clip` keyword-only), and may be picked several times. A raw cost `x` (per round position for the three unit
constraints, the single total for the seven globals — the granularity of `exponent`) is
shaped as `sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which lowers the penalty of
every round below the offset, an empty one included); `offset=0` is the plain power.
Both apply per round position (or to a global's one total), not to the sum over the
rounds. `clip=False` with an offset is refused on hard (`team_conflict_cost` and the other
three hard globals by default), because hard 0 must mean no violation.
Each pick is its own instance with its own knobs, its level included, and the instances
add up:

```python
from optimaru.sports_league_scheduling.constraints import AwayToAwayHopCost, get_constraints

sl.solve(schedule, constraints=[
    *get_constraints(),                                        # the defaults
    AwayToAwayHopCost(weight=0.01, exponent=2, offset=1000),  # + 0.01 x (km beyond 1000)^2
])
```

`sl.constraint_costs(schedule, picks)` keys the second pick `away_to_away_hop_cost#2`. The
CP-SAT reference takes integral offsets.

## Build / verify

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=sports_league_scheduling && cmake --build build -j4
./build/sports_league_scheduling/sls_test         # constraint hand checks + incremental==full over 3000 moves + ScoreChecks::Full solve
./build/sports_league_scheduling/sls_plugin_test  # plugin assembly == monolith, raw costs by hand, catalog, overrides
./build/sports_league_scheduling/sls_demo 10      # the demo league (14 teams/32 rounds/182 matches), 10 s solve, checks 0hard

OPTIMARU_USE_CASES=sports_league_scheduling uv sync && uv run pytest -q tests/sports_league_scheduling
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is `LeagueScheduleModel.from_core(schedule)` or that model as a JSON dict. `solve` takes the engine object, so convert the result with `to_core()`:

```python
from optimaru.sports_league_scheduling.models import LeagueScheduleModel

problem = LeagueScheduleModel.from_core(sl.demo_schedule())
solved = LeagueScheduleModel.from_core(schedule)                # `schedule` solved above
restart = sl.with_initial_solution(problem, solved).to_core()
sl.solve(restart, time_limit_seconds=5.0, random_seed=43)
print(restart.score)
```

Matches are matched by id. A match the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. `round` is a position in `rounds`, not `Round.index`, so a solution that lists different rounds, or the same ones in another order, raises `ValueError`. So do unknown ids and out-of-range values, before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the
engine is `optimaru/sports_league_scheduling/cpp/include/sls/solver.hpp` + `optimaru/sports_league_scheduling/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/sports_league_scheduling/cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one new
constraint through this use case's files in six steps: changed from Python, written as a
component, tested, made cheaper per move, moved to the default fast path and measured.

## Benchmark

[`benchmarks/sports-league-scheduling`](../../../benchmarks/sports-league-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.
