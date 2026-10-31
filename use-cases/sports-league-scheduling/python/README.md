# sports_league — Sports League Scheduling (C++ engine + pybind11)

The `sports-league-scheduling` example model (Apache-2.0; see the repository NOTICE)
C++ port (`../cpp`, namespace `sls`) for use from Python.
Assigns a round (`Round`) to every match (`Match`) of the league — `Match.round`
is the planning variable (-1 = unassigned; the value is a position in the `rounds` list).

## Install / test

```sh
uv venv
uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

```python
import sports_league as sl

schedule = sl.demo_schedule()  # reproduces Java DemoDataGenerator (14 teams/32 rounds/182 matches)
sl.solve(schedule, time_limit_seconds=10.0, random_seed=42, stop_at_first_feasible=True)
print(schedule.score)  # e.g. 0hard/-218994soft (value at the first feasible stop)
print(sum(sl.constraint_costs(schedule)["away_to_away_hop_cost"]["costs"]))  # raw km
```

- `solve(schedule, *, time_limit_seconds, random_seed, parallel_solver_count,
  stop_at_first_feasible, on_new_best, config, constraints)` — solves in place and
  returns it. `config` accepts a `SolverConfig` or a `models.SolveConfig`;
  `constraints` picks, re-weights or flips constraints (`sports_league.constraints`).
- `calculate_score` — full recompute of all 10 constraints at the default weights.
- `constraint_costs` — the raw cost of every constraint (one cost per round position
  for the three unit constraints, one total for the seven globals), before weight,
  exponent and level; `sports_league.constraints.score()` re-prices it in pure Python.
  This is the only per-constraint surface (the 0.1.x `Breakdown` /
  `compute_breakdown()` were removed in 0.2.0).
- `constraint_info` / `list_constraints` — the catalog (name, default level, default
  weight, flippable, reward, default_enabled), read off the C++ components.
- Serialization is the pydantic v2 mirror in `sports_league.models`
  (`LeagueScheduleModel.from_core / to_core`, `SolveConfig`).

## Score mapping

The Java original already uses `HardSoftScore`, so it maps identically onto the engine's
`HardSoftScore(int64 hard, double soft)`: hard = minus the violation count (0 == feasible),
soft = minus the travel distance (km) plus workday derby penalties (1000 per match) (soft is
always integer-valued, held in the engine's double field).

## Constraints (same semantics as Java `SportsLeagueSchedulingConstraintProvider`)

In `list_constraints()` order (the three unit constraints first, then the globals); the
level and weight are the defaults `constraint_info()` reports:

| # | Constraint (Python name) | Level | Weight | What it measures (raw cost; 0 = no violation) |
|---|------|------|------|------|
| 1 | `start_to_away_hop_cost` | soft | 1 | dist(away, home) per match when there is no previous round index |
| 2 | `away_to_end_hop_cost` | soft | 1 | dist(home, away) per match when there is no next round index |
| 3 | `workday_classic_matches_cost` | soft | 1000 | classic matches (derbies) in a non-weekend/holiday round |
| 4 | `team_conflict_cost` | hard | 1 | pairs of matches in the same round index sharing a team (once per pair) |
| 5 | `consecutive_home_matches_cost` | hard | 1 | when home matches cover 4+ distinct consecutive round indices, the run length n (duplicates on one index count as 1) |
| 6 | `consecutive_away_matches_cost` | hard | 1 | the same for away |
| 7 | `repeat_match_on_next_day_cost` | hard | 1 | matches whose return fixture is in the next round index (once per match) |
| 8 | `home_to_away_hop_cost` | soft | 1 | for consecutive-index pairs with m2.away == m1.home, dist(m1.home, m2.home) |
| 9 | `away_to_away_hop_cost` | soft | 1 | if m2.away == m1.away, dist(m1.home, m2.home) |
| 10 | `away_to_home_hop_cost` | soft | 1 | if m2.home == m1.away, dist(m1.home, m1.away) |

Each spec takes `weight=None` (the default above), `hard=None` (the default level;
`True` = hard, `False` = soft) and `exponent=1.0` (applied per round position for the
three unit constraints, on the one total for the seven globals):

```python
from sports_league.constraints import WorkdayClassicMatchesCost, get_constraints, score

sl.constraint_info()
# [ConstraintInfo(name='start_to_away_hop_cost', default_level='soft', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...]
sl.solve(schedule, constraints=get_constraints(
    [*get_constraints()[:2], WorkdayClassicMatchesCost(weight=10_000), *get_constraints()[3:]]))
raw = sl.constraint_costs(schedule)       # raw costs, re-priced in pure Python:
score(raw), score(raw, [WorkdayClassicMatchesCost(hard=True)])
```

**0.2.0:** the names gained the `_cost` suffix (`start_to_away_hop` ->
`start_to_away_hop_cost`, ...), `matches_on_same_day` became `team_conflict_cost` and
`classic_matches` became `workday_classic_matches_cost` (spec classes `MatchesOnSameDay` ->
`TeamConflictCost`, `ClassicMatches` -> `WorkdayClassicMatchesCost`, the others gained a
`Cost` suffix). `Breakdown` and `compute_breakdown()` were removed; read
`constraint_costs()` instead (`Breakdown.classic_match_count` is
`sum(constraint_costs(s)["workday_classic_matches_cost"]["costs"])`).

Round adjacency is all based on `Round.index`, so with sparse indices (e.g. 0,1,4) the
gap is treated as "no round" (the same as Java's `ifNotExists(Round)`).

## Demo data

`demo_schedule()` reproduces the Java `DemoDataGenerator`: the measured distance matrix
of 14 South American teams, 32 rounds, 182 matches over all ordered pairs, and a 5% classic
draw implementing the `java.util.Random(0)` LCG exactly (+ symmetric closure, 18 matches in
total). The only difference is the weekend rounds — Java uses `LocalDate.now()`, while here
round 0 is pinned to a Wednesday for determinism (the same as the C++ demo `sls_demo`).
