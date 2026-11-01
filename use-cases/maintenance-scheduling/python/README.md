# maintenance_scheduling (Python)

The pybind11 package of the maintenance-scheduling example-model port (`../cpp`).
For the detailed domain/constraint description see [`../README.md`](../README.md).

## Install / test

```sh
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

```python
import maintenance_scheduling as mnt

schedule = mnt.generate_demo_data("SMALL")  # reproduces the Java demo
mnt.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(schedule.score, schedule.score.is_feasible())

for i, job in enumerate(schedule.jobs):
    crew = schedule.crews[schedule.crew_index_of(job.crew_date)]
    print(job.name, crew.name, schedule.job_start_date(i), schedule.job_end_date(i))
```

Score (HardSoftScore), at the default weights:

- hard = -(`min_start_date_cost` + `max_end_date_cost` + `crew_conflict_cost`), each x1
- soft = -(`before_ideal_end_date_cost` x1 + `after_ideal_end_date_cost` x1,000,000 +
  `tag_conflict_cost` x1,000)

The constraint names carry a `_cost` suffix since 0.2.0 (`crew_conflict` ->
`crew_conflict_cost`, ...; the job fields `min_start_date` / `max_end_date` keep their
names). Pick, re-weight or flip constraints with `maintenance_scheduling.constraints`, and read
the defaults with `mnt.constraint_info()`. Each spec takes `(weight=None, level=None,
exponent=1.0)` and the keyword-only `offset=0.0` / `clip=True`, and a constraint may be
picked several times, each pick with its own knobs. `level` is `"hard"` or `"soft"`
(`Level.HARD` / `Level.SOFT`); the domain has no medium level, so `level="medium"` raises
`ValueError`:

```python
from maintenance_scheduling.constraints import TagConflictCost, get_constraints, score

mnt.constraint_info()
# [ConstraintInfo(name='min_start_date_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...]
mnt.solve(schedule, time_limit_seconds=1.0, constraints=[
    *get_constraints()[:5], TagConflictCost(weight=10_000),
])                                        # tag conflicts x10
raw = mnt.constraint_costs(schedule)      # raw costs, re-priced in pure Python:
score(raw), score(raw, [TagConflictCost(exponent=2.0)])
# offset / clip shift the shaping (clip=True: only the tag-conflict days beyond 10 count).
score(raw, [TagConflictCost(), TagConflictCost(weight=100, exponent=2, offset=10)])
```

`clip=False` shapes `x - offset` on every (start date, crew) cell, an empty one included
(per cell, not over the sum), so it is refused on hard, where hard 0 must mean no
violation: `ValueError: constraint 'min_start_date_cost': clip=False with a non-zero offset
cannot be hard, because hard 0 must mean no violation; use clip=True (a threshold) or
another level`. On soft it is allowed.

Dates are epoch day integers (Java `LocalDate.toEpochDay`). To work with `datetime.date`,
use the pydantic mirror (optional dependency `pydantic`):

```python
from maintenance_scheduling.models import MaintenanceScheduleModel

model = MaintenanceScheduleModel.from_core(schedule)  # engine -> pydantic
schedule2 = model.to_core()  # pydantic -> engine
json_text = model.model_dump_json()
```

Type stubs are in `maintenance_scheduling/_core.pyi`; the high-level entry point is
`maintenance_scheduling.solve()` (config/time_limit/seed/
stop_at_first_feasible/on_new_best).
