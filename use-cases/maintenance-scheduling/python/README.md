# maintenance_scheduling (Python)

The pybind11 package of the Timefold quickstart maintenance-scheduling port (`../cpp`).
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
