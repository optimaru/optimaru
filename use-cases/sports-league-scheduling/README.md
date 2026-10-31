# Sports League Scheduling (C++ / Python)

The `sports-league-scheduling` example model (Apache-2.0; see the repository NOTICE)
ported onto the generic C++ engine in `include/optimaru/`. Assigns a round (`Round`) to
every match (`Match`) of the league (one BASIC variable: `Match::round`).

- `cpp/` — domain (`sls::LeagueSchedule`), full-recompute scorer, O(delta)
  incremental scorer, a thin solver wrapper (LA + ALNS), demo (`sls_demo`),
  tests (`sls_test`, `sls_plugin_test`).
- `include/sls/constraints/` — one component per constraint (+ `common.hpp`):
  `team_conflict_cost.hpp`, `consecutive_home_matches_cost.hpp`,
  `consecutive_away_matches_cost.hpp`, `repeat_match_on_next_day_cost.hpp`,
  `start_to_away_hop_cost.hpp`, `home_to_away_hop_cost.hpp`,
  `away_to_away_hop_cost.hpp`, `away_to_home_hop_cost.hpp`,
  `away_to_end_hop_cost.hpp`, `workday_classic_matches_cost.hpp`;
  `include/sls/constraints.hpp` assembles the default set (`defaultConstraints()`).
- `python/` — pybind11 package `sports_league` (with the pydantic mirror, demo generator
  and type stubs). Details in `python/README.md`.

## Build / verify

```sh
cd cpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/sls_test         # constraint hand checks + incremental==full over 3000 moves + FULL_ASSERT solve
./build/sls_plugin_test  # plugin assembly == monolith, raw costs by hand, catalog, overrides
./build/sls_demo 10      # reproduces the Java demo (14 teams/32 rounds/182 matches), 10 s solve, checks 0hard

cd ../python && uv venv && uv pip install -e . pytest pydantic && uv run pytest -q
```

## Score mapping

The Java original already uses `HardSoftScore`, so it maps identically onto the engine's
`HardSoftScore(int64 hard, double soft)` (soft stored as an integer value in the double).
The 10 constraints have the same semantics as Java `SportsLeagueSchedulingConstraintProvider`
(more detail in `python/README.md` and the comments in `cpp/include/sls/
score_calculator.hpp`):

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `start_to_away_hop_cost` | `StartToAwayHopCost` | soft | 1 | per round position: km the away teams travel to its matches when no earlier round index exists |
| `away_to_end_hop_cost` | `AwayToEndHopCost` | soft | 1 | per round position: km the away teams travel home after its matches when no later round index exists |
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
flippable (`hard=True` / `hard=False`), none is a reward and all ten are scored by default
(`default_enabled`); `sports_league.constraint_info()` reports exactly this table from the
C++ components (`kDefaultLevel` / `kDefaultWeight`). The consecutive rules charge the
whole run length (a run of 4 costs 4), as the Java provider does.

**0.2.0:** the constraints were renamed — `matches_on_same_day` -> `team_conflict_cost`,
`classic_matches` -> `workday_classic_matches_cost`, and every other name gained the
`_cost` suffix (`consecutive_home_matches` -> `consecutive_home_matches_cost`, ...,
`away_to_end_hop` -> `away_to_end_hop_cost`); the spec classes, module-level default
instances, C++ structs/headers and registry names follow (`MatchesOnSameDay` ->
`TeamConflictCost`, `"matchesOnSameDay"` -> `"teamConflictCost"`, ...). The Python
`Breakdown` / `compute_breakdown()` are gone: `constraint_costs()` is the only
per-constraint surface. The CP-SAT reference reads its defaults from `constraint_info()`,
which fixes its workday-derby weight (it used 1 instead of the engine's 1000). Raw costs
and the default score are unchanged.

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
`cpp/include/sls/constraints/` (currently `team_conflict_cost`,
`consecutive_home_matches_cost`, `consecutive_away_matches_cost`,
`repeat_match_on_next_day_cost`, `start_to_away_hop_cost`, `home_to_away_hop_cost`,
`away_to_away_hop_cost`, `away_to_home_hop_cost`, `away_to_end_hop_cost`,
`workday_classic_matches_cost`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/sls/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set (its `info()` row is then in
   `constraint_info()`);
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` (`kHardWeight<X>` / `kSoftWeight<X>`) — `sls_plugin_test` checks they
   agree bit for bit — and a positive and a zero hand-calculated case to `sls_test` /
   `sls_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/sports_league/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/sports-league-scheduling/cpsat`](../../benchmarks/sports-league-scheduling/cpsat)
   (a plugin subclassing the spec; defaults come from `constraint_info()`) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/sports-league-scheduling`](../../benchmarks/sports-league-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.
