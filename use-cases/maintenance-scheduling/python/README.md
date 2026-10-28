# maintenance_scheduling (Python)

Timefold quickstart maintenance-scheduling 포트(`../cpp`)의 pybind11
패키지. 자세한 도메인/제약 설명은 [`../README.md`](../README.md) 참고.

## 설치 / 테스트

```sh
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## 사용

```python
import maintenance_scheduling as mnt

schedule = mnt.generate_demo_data("SMALL")  # Java 데모 재현
mnt.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(schedule.score, schedule.score.is_feasible())

for i, job in enumerate(schedule.jobs):
    crew = schedule.crews[schedule.crew_index_of(job.crew_date)]
    print(job.name, crew.name, schedule.job_start_date(i), schedule.job_end_date(i))
```

날짜는 epoch day 정수(Java `LocalDate.toEpochDay`)다. `datetime.date` 로
다루려면 pydantic 미러를 쓴다 (선택 의존성 `pydantic`):

```python
from maintenance_scheduling.models import MaintenanceScheduleModel

model = MaintenanceScheduleModel.from_core(schedule)  # 엔진 -> pydantic
schedule2 = model.to_core()  # pydantic -> 엔진
json_text = model.model_dump_json()
```

타입 스텁은 `maintenance_scheduling/_core.pyi`, 고수준 진입점은
`maintenance_scheduling.solve()` (config/time_limit/seed/
stop_at_first_feasible/on_new_best).
