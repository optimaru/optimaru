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
print(sl.compute_breakdown(schedule).away_to_away_hop)
```

- `solve(schedule, *, time_limit_seconds, random_seed, parallel_solver_count,
  stop_at_first_feasible, on_new_best, config)` — solves in place and returns it.
  `config` accepts a `SolverConfig` or a `models.SolveConfig`.
- `calculate_score` / `compute_breakdown` — full recompute of all 10 constraints
  and per-constraint totals.
- Serialization is the pydantic v2 mirror in `sports_league.models`
  (`LeagueScheduleModel.from_core / to_core`, `SolveConfig`).

## Score mapping

The Java original already uses `HardSoftScore`, so it maps identically onto the engine's
`HardSoftScore(int64 hard, double soft)`: hard = minus the violation count (0 == feasible),
soft = minus the travel distance (km) plus classic penalties (1000 per match) (soft is
always integer-valued, held in the engine's double field).

## Constraints (same semantics as Java `SportsLeagueSchedulingConstraintProvider`)

| # | Constraint | Level | What it measures |
|---|------|------|------|
| 1 | matchesOnSameDay | hard | -1 per pair of matches in the same round index sharing a team |
| 2 | 4+ consecutive home | hard | when home matches cover 4+ distinct consecutive round indices, -n for the run length (duplicates on one index count as 1) |
| 3 | 4+ consecutive away | hard | the same for away |
| 4 | repeatMatchOnNextDay | hard | -1 per match whose return fixture is in the next round index |
| 5 | startToAwayHop | soft | -dist(away, home) when there is no previous round index |
| 6 | homeToAwayHop | soft | for consecutive-index pairs with m2.away == m1.home, -dist(m1.home, m2.home) |
| 7 | awayToAwayHop | soft | if m2.away == m1.away, -dist(m1.home, m2.home) |
| 8 | awayToHomeHop | soft | if m2.home == m1.away, -dist(m1.home, m1.away) |
| 9 | awayToEndHop | soft | -dist(home, away) when there is no next round index |
| 10 | classicMatches | soft | -1000 per classic match in a non-weekend/holiday round |

Round adjacency is all based on `Round.index`, so with sparse indices (e.g. 0,1,4) the
gap is treated as "no round" (the same as Java's `ifNotExists(Round)`).

## Demo data

`demo_schedule()` reproduces the Java `DemoDataGenerator`: the measured distance matrix
of 14 South American teams, 32 rounds, 182 matches over all ordered pairs, and a 5% classic
draw implementing the `java.util.Random(0)` LCG exactly (+ symmetric closure, 18 matches in
total). The only difference is the weekend rounds — Java uses `LocalDate.now()`, while here
round 0 is pinned to a Wednesday for determinism (the same as the C++ demo `sls_demo`).
