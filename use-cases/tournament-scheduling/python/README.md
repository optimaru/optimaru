# tournament_scheduling

The `tournament-scheduling` example model (Apache-2.0; see the repository NOTICE)
(Java/Quarkus) ported onto the C++ engine in `include/optimaru/`. Assigns teams to the daily
match slots (TeamAssignment) — the C++ domain/calculators are in `../cpp`; this package
holds their pybind11 bindings (`tournament_scheduling._core`) and the pydantic mirror.

## Constraints (same semantics as the Java ConstraintProvider)

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `team_conflict_cost` | `TeamConflictCost` | hard | 1 | one match per team per day: 1 per pair of same-team assignments on one day |
| `team_unavailability_cost` | `TeamUnavailabilityCost` | hard | 1 | 1 per fact when an unavailable (team, day) has any assignment |
| `assignment_count_imbalance_cost` | `AssignmentCountImbalanceCost` | medium | 1 | LoadBalance unfairness of the assignment counts per team |
| `confrontation_count_imbalance_cost` | `ConfrontationCountImbalanceCost` | soft | 1 | LoadBalance unfairness of the head-to-head counts per team pair |

`tournament_scheduling.constraint_info()` returns exactly these rows (`ConstraintInfo`:
`name`, `default_level`, `default_weight`, `level_flippable`, `reward`, `default_enabled`),
read off the C++ components' `kDefaultLevel` / `kDefaultWeight`;
`constraint_costs(schedule)` returns the raw per-unit costs (a team for the two hard
constraints, one total for the two imbalance constraints) with that metadata and the
domain's `medium_fold` (`{"kind": "into_soft", "unit": 1e6}`) on every entry.
`tournament_scheduling.constraints` holds one spec per row (`TeamConflictCost`, ...) and the
module-level default instances (`team_conflict_cost` is `TeamConflictCost()`, ...). Every
spec takes `weight=None` (the default above, in units of its level), `level=None` (the
default level, otherwise a `Level` or its name), `exponent=1.0` and the keyword-only
`offset=0.0` / `clip=True` (`clip=True` counts only what lies beyond the offset,
`clip=False` shapes `x - offset` as it is, and is refused with an offset on hard, because
hard 0 must mean no violation; the scoped `team_unavailability_cost` lists the teams it
evaluates as `"units"`). Any constraint may take any of the three levels: the
medium constraint moved to hard costs 1 hard per raw unit (the total, rounded), moved to
soft 1 soft per raw unit, a hard or soft constraint moved to medium 1 medium unit per raw
unit, and an explicit weight is in units of the level in effect. A constraint may be picked
several times; each pick is its own instance with its own knobs, its level
included, the instances add up, and
`constraint_costs(schedule, picks)` keys the k-th pick `<name>#<k>`.

"Unfairness" is the load-balance unfairness of the reference model:
`sqrt(sum(load^2) - sum(load)^2 / n)` rounded to 6 significant digits (HALF_EVEN).
`n` is the number of items with load ≥ 1 (the Java original has no complement, so teams
without assignments / pairs without head-to-heads are not counted).

## Score mapping (HardMediumSoftBigDecimalScore → Score)

The engine score is `Score(hard int64, soft double)`, so Java's three levels are
folded into two (the same as the `cpp/include/ts/domain.hpp` comments):

```
hard = Java hard as-is                         (0 == feasible)
soft = -(medium unfairness * 1e6 + soft unfairness)   # MEDIUM_SCALE = 1e6
```

The fold is the domain's `kMediumFold = intoSoft(1e6)` (`cpp/include/ts/domain.hpp`), applied
by the calculators, not carried by the medium constraint's weight.

medium/soft unfairness are 6-significant-digit decimals, so at this domain's instance
scale medium always dominates soft (level priority preserved).
E.g. the best demo solution using all 7 teams is `0hard/-1195232.56soft` == at Java
levels `0hard/-1.19523medium/-2.56soft`.

### Note: the objective's global optimum uses fewer teams

Because `n` counts only teams with load ≥ 1, emptying a team entirely improves medium.
In the demo (72 assignments / 7 teams), using all 7 teams gives a minimum medium of
`sqrt(10/7)=1.19523`, but 6 teams x 12 assignments give medium **0**. The solver finds this
global optimum, so the demo result is `0hard/-1.54919soft` (medium 0, soft 1.54919) and one
team is left with 0 matches. This is not a calculator bug but the optimum of the ported Java
objective as-is. If "every team plays" is a domain requirement, fix the constraints, not
the solver (add a complement term or a hard minimum-assignments-per-team constraint).

## Usage

```bash
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

```python
import tournament_scheduling as ts
from tournament_scheduling.models import demo_data

schedule = demo_data()  # the demo instance: 7 teams, 18 days x 4 slots, 12 unavailable days
result = ts.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_soft)  # 0, -1.54919 (feasible, 6 teams x 12 assignments)
```

`solve` accepts both the pydantic model (`models.TournamentScheduleModel`) and `_core`
objects. Pin an assignment with `TeamAssignmentModel.pinned=True`
(Java `@PlanningPin`); to stop at the first feasible use
`stop_at_first_feasible=True`.

## C++ demo/tests

```bash
cd ../cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/ts_test    # hand-computed constraint cases + 3000 random moves incremental==full + ScoreChecks::Full solve
./build/ts_plugin_test  # default assembly == monolith, raw costs by hand, catalog, medium knobs
./build/ts_demo 10 # the demo instance, 10 s solve, asserts feasible (0hard)
```
