# optimaru.sports_league_scheduling — Sports League Scheduling (C++ engine + pybind11)

The `sports-league-scheduling` example model (Apache-2.0; see the repository NOTICE)
C++ port (`optimaru/sports_league_scheduling/cpp`, namespace `sls`) for use from Python.
Assigns a round (`Round`) to every match (`Match`) of the league — `Match.round`
is the decision variable (-1 = unassigned; the value is a position in the `rounds` list).

## Install / test

```sh
OPTIMARU_USE_CASES=sports_league_scheduling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/sports_league_scheduling
```

## Usage

```python
from optimaru import sports_league_scheduling as sl

schedule = sl.demo_schedule()  # the demo instance (14 teams/32 rounds/182 matches)
sl.solve(schedule, time_limit_seconds=10.0, random_seed=42, stop_at_first_feasible=True)
print(schedule.score)  # e.g. 0hard/-218994soft (value at the first feasible stop)
print(sum(sl.constraint_costs(schedule)["away_to_away_hop_cost"]["costs"]))  # raw km
```

- `solve(schedule, *, time_limit_seconds, random_seed, parallel_solver_count,
  stop_at_first_feasible, on_new_best, config, constraints)` — solves in place and
  returns it. `config` accepts a `SolverConfig` or a `models.SolveConfig`;
  `constraints` picks, re-weights or flips constraints (`optimaru.sports_league_scheduling.constraints`).
- `calculate_score` — full recompute of all 10 constraints at the default weights.
- `constraint_costs` — the raw cost of every constraint (one cost per round position
  for the three unit constraints, one total for the seven globals), before weight,
  exponent and level; `optimaru.sports_league_scheduling.constraints.score()` re-prices it in pure Python.
  This is the per-constraint surface (e.g. the number of workday classic matches is
  `sum(constraint_costs(s)["workday_classic_matches_cost"]["costs"])`).
- `constraint_info` / `list_constraints` — the catalog (name, default level, default
  weight, flippable, reward, default_enabled), read off the C++ components.
- Serialization is the pydantic v2 mirror in `optimaru.sports_league_scheduling.models`
  (`LeagueScheduleModel.from_core / to_core`, `SolveConfig`).

## Score mapping

The Java original already uses `Score`, so it maps identically onto the engine's
`Score(int64 hard, double soft)`: hard = minus the violation count (0 == feasible),
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

Each spec takes `weight=None` (the default above), `level=None` (the default level,
otherwise `"hard"` or `"soft"`, or `Level.HARD` / `Level.SOFT`; the domain has no
medium level, so `level="medium"` raises `ValueError`), `exponent=1.0` (applied per
round position for the three unit constraints, on the one total for the seven globals)
and the keyword-only `offset=0.0` / `clip=True`; a constraint may be picked several
times, each pick with its own knobs:

```python
from optimaru.sports_league_scheduling.constraints import AwayToAwayHopCost, WorkdayClassicMatchesCost, get_constraints, score

sl.constraint_info()
# [ConstraintInfo(name='start_to_away_hop_cost', default_level='soft', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...]
sl.solve(schedule, time_limit_seconds=1.0, constraints=[
    *get_constraints()[:2], WorkdayClassicMatchesCost(weight=10_000), *get_constraints()[3:],
])
raw = sl.constraint_costs(schedule)       # raw costs, re-priced in pure Python:
score(raw), score(raw, [WorkdayClassicMatchesCost(level="hard")])
# offset / clip shift the shaping (clip=True: only the km beyond 1000 count).
score(raw, [AwayToAwayHopCost(), AwayToAwayHopCost(weight=0.01, exponent=2, offset=1000)])
```

`clip=False` shapes `x - offset` on every round position of a unit constraint, an empty
one included (per round, not over the sum), and on a global's one total. It is refused on
hard, where hard 0 must mean no violation: `ValueError: constraint 'team_conflict_cost':
clip=False with a non-zero offset cannot be hard, because hard 0 must mean no violation;
use clip=True (a threshold) or another level`. On soft it is allowed.

Round adjacency is all based on `Round.index`, so with sparse indices (e.g. 0,1,4) the
gap is treated as "no round" (the same as Java's `ifNotExists(Round)`).

## Demo data

`demo_schedule()` builds the demo league, modelled on the quickstart's demo: the measured
distance matrix of 14 South American teams, 32 rounds, 182 matches over all ordered pairs, and
a 5% classic draw from a seeded `random.Random` (+ symmetric closure, 18 matches in total).
Round 0 is pinned to a Wednesday, so the instance is the same on every run. (The C++ demo
`sls_demo` draws its classic matches with `std::mt19937_64`, so its instance differs.)
