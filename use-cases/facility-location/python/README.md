# facility_location (Python)

Timefold quickstart facility-location 포팅(`../cpp`)의 pybind11 패키지.
수요처(Consumer)마다 시설(Facility) 하나를 고른다 — `Consumer.facility` 가
유일한 planning variable (시설 인덱스, -1 = 미배정).

Score (HardSoftScore):

- hard = -(사용 시설별 `max(0, 배정 수요 - 용량)` 합)
- soft = -(2 x 사용 시설 설치비 합 + 6 x 수요처-시설 거리(m) 합)
  - 거리 가중 6 = Java 솔루션의 `ConstraintWeightOverrides` 값 (provider
    기본 5). `problem.distance_from_facility_weight` 로 변경 가능.

## 설치 / 테스트

```bash
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## 사용

```python
import facility_location as fl

facilities = [fl.Facility("F1", fl.Location(51.5, -0.1), 50_000, 150), ...]
consumers = [fl.Consumer("C1", fl.Location(51.52, -0.12), 15), ...]
problem = fl.FacilityLocationProblem(facilities, consumers)

fl.solve(problem, time_limit_seconds=5.0, random_seed=42)  # 제자리 solve
print(problem.score)  # 0hard/-...soft
print(problem.assignment())  # 수요처별 시설 인덱스
print(problem.total_cost(), problem.total_distance_meters())
```

세부 제어는 `fl.SolverConfig()` (시간 예산, 멀티스타트, ALNS 노브,
`on_new_best` 콜백, `best_score_limit` 조기 종료 등) 를 만들어
`fl.Solver(cfg).solve(problem)` 또는 `fl.solve(problem, config=cfg)` 로.

pydantic v2 미러(JSON 직렬화)는 `facility_location.models`:

```python
from facility_location.models import FacilityLocationProblemModel

model = FacilityLocationProblemModel.from_core(problem)
problem2 = FacilityLocationProblemModel.model_validate_json(model.model_dump_json()).to_core()
```

주의: `problem.consumers[i]` 접근은 복사본을 돌려준다 — 배정 일괄 쓰기는
`problem.set_assignment([...])` 를 쓸 것.
