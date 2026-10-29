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

schedule = fp.generate_demo_data()  # Java demo (5 lines, 100 jobs)
fp.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(schedule.score)  # e.g. 0hard/-1298medium/-109664360soft

for line in schedule.lines:
    print(line.name, [schedule.jobs[j].name for j in line.jobs])

# Per-constraint penalty breakdown (one penalty per constraint)
b = fp.analyze(schedule)
print(b.max_end_date_time, b.operator_cleaning_conflict, b.minimize_makespan)
```

pydantic serialization:

```python
from food_packaging.models import PackagingScheduleModel

model = PackagingScheduleModel.from_core(schedule)  # engine -> pydantic
text = model.model_dump_json()
schedule2 = PackagingScheduleModel.model_validate_json(text).to_core()
```

## Score semantics

Java uses `HardMediumSoftScore` and the engine `HardSoftScore(int64, double)`, so the
mapping is `engine.soft = medium * 10^10 + soft`. `Score` exposes all three levels as-is,
so user code never sees the mapping. For the order-preservation conditions and domain
details see `../cpp/README.md` and the `fp/domain.hpp` comments.
`Line.operator` (Java's basic planning variable) is reassigned by the solver with hill
climbing between engine legs — exactly equivalent to Java with operators >= lines,
an alternating heuristic otherwise (caveat).
