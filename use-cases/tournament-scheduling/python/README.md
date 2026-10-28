# tournament_scheduling

Timefold quickstart [tournament-scheduling](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/tournament-scheduling)
(Java/Quarkus) ported onto the C++ engine in `include/optimaru/`. Assigns teams to the daily
match slots (TeamAssignment) — the C++ domain/calculators are in `../cpp`; this package
holds their pybind11 bindings (`tournament_scheduling._core`) and the pydantic mirror.

## Constraints (same semantics as the Java ConstraintProvider)

| Name | Level | What it measures |
|------|------|------|
| oneAssignmentPerDatePerTeam | Hard | one match per team per day (-1 per pair of same-team assignments on one day) |
| unavailabilityPenalty | Hard | -1 per fact when an unavailable (team, day) has any assignment |
| fairAssignmentCountPerTeam | Medium | fair assignment counts per team (LoadBalance unfairness) |
| evenlyConfrontationCount | Soft | even head-to-head counts per team pair (LoadBalance unfairness) |

"Unfairness" is exactly Timefold `LoadBalance::unfairness`:
`sqrt(sum(load^2) - sum(load)^2 / n)` rounded to 6 significant digits (HALF_EVEN).
`n` is the number of items with load ≥ 1 (the Java original has no complement, so teams
without assignments / pairs without head-to-heads are not counted).

## Score mapping (HardMediumSoftBigDecimalScore → HardSoftScore)

The engine score is `HardSoftScore(hard int64, soft double)`, so Java's three levels are
folded into two (the same as the `cpp/include/ts/domain.hpp` comments):

```
hard = Java hard as-is                         (0 == feasible)
soft = -(medium unfairness * 1e6 + soft unfairness)   # MEDIUM_SCALE = 1e6
```

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

schedule = demo_data()  # reproduces Java DemoDataGenerator: 7 teams, 18 days x 4 slots, 12 unavailable days
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
./build/ts_test    # hand-computed constraint cases + 3000 random moves incremental==full + FULL_ASSERT solve
./build/ts_demo 10 # reproduces the Java demo, 10 s solve, asserts feasible (0hard)
```
