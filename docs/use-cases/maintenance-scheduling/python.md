# optimaru.maintenance_scheduling (Python)

The pybind11 package of the maintenance-scheduling use case (`optimaru/maintenance_scheduling/cpp`).
For the detailed domain/constraint description see [`../README.md`](README.md).

## Install and test

```sh
OPTIMARU_USE_CASES=maintenance_scheduling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/maintenance_scheduling
```

## Usage

```python
from optimaru import maintenance_scheduling as mnt

schedule = mnt.generate_demo_data("SMALL")  # the demo instance
mnt.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(schedule.score, schedule.score.is_feasible())

for i, job in enumerate(schedule.jobs):
    crew = schedule.crews[schedule.crew_index_of(job.crew_date)]
    print(job.name, crew.name, schedule.job_start_date(i), schedule.job_end_date(i))
```

Score, at the default weights:

- hard = -(`min_start_date_cost` + `max_end_date_cost` + `crew_conflict_cost`), each x1
- soft = -(`before_ideal_end_date_cost` x1 + `after_ideal_end_date_cost` x1,000,000 +
  `tag_conflict_cost` x1,000)

The constraint names carry a `_cost` suffix (`crew_conflict_cost`, ...), which keeps them
apart from the job fields `min_start_date` / `max_end_date`. Pick, re-weight or flip constraints with `optimaru.maintenance_scheduling.constraints`, and read
the defaults with `mnt.constraint_info()`. Each spec takes `(weight=None, level=None,
exponent=1.0)` and the keyword-only `offset=0.0` / `clip=True`, and a constraint may be
picked several times, each pick with its own knobs. `level` is `"hard"` or `"soft"`
(`Level.HARD` / `Level.SOFT`); the domain has no medium level, so `level="medium"` raises
`ValueError`:

```python
from optimaru.maintenance_scheduling.constraints import TagConflictCost, get_constraints, score

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

Dates are epoch day integers (days since 1970-01-01). To work with `datetime.date`,
use the pydantic mirror:

```python
from optimaru.maintenance_scheduling.models import MaintenanceScheduleModel

model = MaintenanceScheduleModel.from_core(schedule)  # engine -> pydantic
schedule2 = model.to_core()  # pydantic -> engine
json_text = model.model_dump_json()
```

Type stubs are in `optimaru/maintenance_scheduling/_core.pyi`; the high-level entry point is
`maintenance_scheduling.solve()` (config/time_limit/seed/
stop_at_first_feasible/on_new_best).

## JSON in, JSON out

`solve_request` takes a pydantic request, `models.MaintenanceScheduleRequest`: the problem model as
`model_input`, with the settings of the solve under the names of `solve`'s keywords
(`time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`,
`full_assert`, and `solver_parameters` for further `SolverConfig` fields by name). It
returns the solved `MaintenanceScheduleModel`, as `solve(model, ...)` does, and leaves the request
unchanged. `constraints=` is the selection `solve` takes.

```python
from optimaru.maintenance_scheduling import solve_request
from optimaru.maintenance_scheduling.models import MaintenanceScheduleRequest

request = MaintenanceScheduleRequest.model_validate_json(text)  # or MaintenanceScheduleRequest(model_input=model, time_limit_seconds=5.0)
solved = solve_request(request)
```
