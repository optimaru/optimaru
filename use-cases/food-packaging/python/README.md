# food_packaging — Python bindings

The pybind11 package of the `food-packaging` example-model C++ port (`../cpp`).
Times are exchanged as **minutes** (int64) from a shared origin, and the pydantic
mirror (`food_packaging.models`) handles conversion to and from `datetime`.

## Install/test

```sh
uv venv
uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

```python
import food_packaging as fp

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
from food_packaging.constraints import UnassignedJobsCost, get_constraints, score

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
from food_packaging.constraints import MaxEndTimeCost

score(raw, [MaxEndTimeCost(), MaxEndTimeCost(exponent=2, offset=30)])  # late minutes + (beyond 30)^2
```

pydantic serialization:

```python
from food_packaging.models import PackagingScheduleModel

model = PackagingScheduleModel.from_core(schedule)  # engine -> pydantic
text = model.model_dump_json()
schedule2 = PackagingScheduleModel.model_validate_json(text).to_core()
```

## Score semantics

Java uses `HardMediumSoftScore` and the engine `Score(int64, double)`, so the
mapping is `engine.soft = medium * 10^10 + soft` (the domain's medium fold; every
`constraint_costs()` entry carries it as `medium_fold`, and
`food_packaging.constraints.score()` returns the folded engine-level `(hard, soft)`).
`Score` exposes all three levels as-is, so user code never sees the mapping. Medium
weights are in medium units (default 1): `level="hard"` on a medium constraint costs 1 hard
per minute, `level="soft"` 1 soft per minute, and `level="medium"` puts a hard or soft
constraint on medium with its weight in medium units. For the order-preservation conditions and domain
details see `../cpp/README.md` and the `fp/domain.hpp` comments.
`Line.operator` (Java's basic planning variable) is reassigned by the solver with hill
climbing between engine legs — exactly equivalent to Java with operators >= lines,
an alternating heuristic otherwise (caveat).
