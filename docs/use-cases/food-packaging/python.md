# optimaru.food_packaging (Python)

The pybind11 package of the food-packaging C++ implementation (`optimaru/food_packaging/cpp`).
Times are exchanged as **minutes** (int64) from a shared origin, and the pydantic
mirror (`optimaru.food_packaging.models`) handles conversion to and from `datetime`.

## Install and test

```sh
OPTIMARU_USE_CASES=food_packaging uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/food_packaging
```

## Usage

```python
from optimaru import food_packaging as fp

schedule = fp.generate_demo_data()  # the demo plant (5 lines, 100 jobs)
fp.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(schedule.score)  # e.g. 0hard/0medium/-96677943soft

for line in schedule.lines:
    print(line.name, [schedule.jobs[j].name for j in line.jobs])

# Per-constraint raw costs (per line, or one total for a global constraint)
raw = fp.constraint_costs(schedule)
print({name: sum(entry["costs"]) for name, entry in raw.items()})
# {'max_end_time_cost': 0.0, 'after_ideal_end_time_cost': ..., 'squared_makespan_cost': ...,
#  'operator_cleaning_conflict_cost': 0.0, 'unassigned_jobs_cost': 0.0}
```

The constraints, their defaults and how to change them:

```python
from optimaru.food_packaging.constraints import UnassignedJobsCost, get_constraints, score

fp.constraint_info()
# [ConstraintInfo(name='max_end_time_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...,
#  ConstraintInfo(name='unassigned_jobs_cost', default_level='medium', default_weight=1.0, ...)]
others = [spec for spec in get_constraints() if spec.name != "unassigned_jobs_cost"]
fp.solve(schedule, time_limit_seconds=1.0, constraints=[*others, UnassignedJobsCost(level="soft")])  # unassigned as soft
score(raw), score(raw, [*others, UnassignedJobsCost(weight=2)])            # re-priced in pure Python
```

| Python name | Default | Raw cost |
|---|---|---|
| `max_end_time_cost` | hard, 1 | minutes past `max_end_time`, per line |
| `after_ideal_end_time_cost` | medium, 1 | minutes past `ideal_end_time`, per line |
| `squared_makespan_cost` | soft, 1 | (line busy minutes)^2, per line |
| `operator_cleaning_conflict_cost` | hard, 1 | overlapping cleaning minutes on lines of one operator (global) |
| `unassigned_jobs_cost` | medium, 1 | production minutes of jobs on no line (global) |

`constraint_costs()` is the per-constraint surface.

Every spec takes `weight=None`, `level=None` (the default level, otherwise a `Level` or its
name: any constraint may take any of the three levels), `exponent=1.0` and the
keyword-only `offset=0.0` / `clip=True` (`clip=True` counts only what lies beyond the
offset, `clip=False` shapes `x - offset` as it is, per line or on a global's total, and is
refused with an offset on hard, because hard 0 must mean no violation). A
constraint may be picked several times; each pick is its own instance with its own
knobs, its level included, the instances add up, and `constraint_costs(schedule, picks)`
keys the k-th pick `<name>#<k>`:

```python
from optimaru.food_packaging.constraints import MaxEndTimeCost

score(raw, [MaxEndTimeCost(), MaxEndTimeCost(exponent=2, offset=30)])  # late minutes + (beyond 30)^2
```

pydantic serialization:

```python
from optimaru.food_packaging.models import PackagingScheduleModel

model = PackagingScheduleModel.from_core(schedule)  # engine -> pydantic
text = model.model_dump_json()
schedule2 = PackagingScheduleModel.model_validate_json(text).to_core()
```

## JSON in, JSON out

`solve_request` takes a pydantic request, `models.PackagingScheduleRequest`: the problem model as
`model_input`, with the settings of the solve under the names of `solve`'s keywords
(`time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`,
`full_assert`, and `solver_parameters` for further `SolverConfig` fields by name). It
returns the solved `PackagingScheduleModel`, as `solve(model, ...)` does, and leaves the request
unchanged. `constraints=` is the selection `solve` takes.

```python
from optimaru.food_packaging import solve_request
from optimaru.food_packaging.models import PackagingScheduleRequest

request = PackagingScheduleRequest.model_validate_json(text)  # or PackagingScheduleRequest(model_input=model, time_limit_seconds=5.0)
solved = solve_request(request)
```

## Score semantics

The score has three levels (hard / medium / soft) and the engine's is `Score(int64, double)`, so the
mapping is `engine.soft = medium * 10^10 + soft` (the domain's medium fold; every
`constraint_costs()` entry carries it as `medium_fold`, and
`optimaru.food_packaging.constraints.score()` returns the folded engine-level `(hard, soft)`).
`Score` exposes all three levels as-is, so user code never sees the mapping. Medium
weights are in medium units (default 1): `level="hard"` on a medium constraint costs 1 hard
per minute, `level="soft"` 1 soft per minute, and `level="medium"` puts a hard or soft
constraint on medium with its weight in medium units. For the order-preservation conditions and domain
details see [`cpp.md`](cpp.md) and the `fp/domain.hpp` comments.
`Line.operator_index` (each line's operator, the second decision next to the job order)
is reassigned by the solver with hill climbing between engine legs — exact when there
are at least as many operators as lines, an alternating heuristic otherwise (caveat).
