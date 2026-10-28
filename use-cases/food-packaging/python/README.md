# food_packaging — Python 바인딩

Timefold quickstart `food-packaging` C++ 포트(`../cpp`)의 pybind11
패키지. 시간은 공통 기준으로부터의 **분**(int64)으로 주고받고, pydantic
미러(`food_packaging.models`)가 `datetime` 과의 변환을 맡는다.

## 설치/테스트

```sh
uv venv
uv pip install -e . pytest pydantic
uv run pytest -q
```

## 사용

```python
import food_packaging as fp

schedule = fp.generate_demo_data()  # Java 데모 (라인 5, 작업 100)
fp.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(schedule.score)  # 예: 0hard/-1298medium/-109664360soft

for line in schedule.lines:
    print(line.name, [schedule.jobs[j].name for j in line.jobs])

# 제약별 페널티 분해 (Timefold SolutionManager.analyze 대응)
b = fp.analyze(schedule)
print(b.max_end_date_time, b.operator_cleaning_conflict, b.minimize_makespan)
```

pydantic 직렬화:

```python
from food_packaging.models import PackagingScheduleModel

model = PackagingScheduleModel.from_core(schedule)  # 엔진 -> pydantic
text = model.model_dump_json()
schedule2 = PackagingScheduleModel.model_validate_json(text).to_core()
```

## 점수 의미론

Java 는 `HardMediumSoftScore`, 엔진은 `HardSoftScore(int64, double)` 라서
`engine.soft = medium * 10^10 + soft` 로 매핑한다. `Score` 는 세 레벨을
그대로 노출하므로 사용자 코드에서는 매핑이 보이지 않는다. 순위 보존
조건과 도메인 상세는 `../cpp/README.md` 와 `fp/domain.hpp` 주석 참고.
`Line.operator`(Java 의 basic planning variable)는 솔버가 엔진 레그
사이에 힐클라이밍으로 재배정한다 — operator 수 >= 라인 수면 Java 와
완전 동치, 아니면 교대 휴리스틱 (caveat).
