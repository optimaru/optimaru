# flight_crew

Timefold quickstart flight-crew-scheduling 의 C++ 포트(`../cpp`)를 pybind11
로 감싼 Python 패키지. 항공편의 승무원 좌석마다 승무원을 고르는 BASIC 변수
문제를 late-acceptance local search + ALNS 로 푼다.

## 설치

```bash
uv venv
uv pip install -e . pytest pydantic
uv run pytest -q
```

## 사용

고수준 API 는 `solve` 하나다. pydantic 모델(또는 같은 형태의 dict)을 받아
풀린 모델을 돌려준다:

```python
from datetime import datetime

import flight_crew as fc
from flight_crew.models import (
    AirportModel, EmployeeModel, FlightAssignmentModel,
    FlightCrewScheduleModel, FlightModel,
)

problem = FlightCrewScheduleModel(
    airports=[AirportModel(code="ICN"), AirportModel(code="NRT")],
    employees=[
        EmployeeModel(id="P1", home_airport="ICN", skills=["Pilot"]),
        EmployeeModel(id="C1", home_airport="ICN", skills=["Flight attendant"]),
    ],
    flights=[
        FlightModel(
            flight_number="OZ102",
            departure_airport="ICN",
            departure_utc_date_time=datetime(2026, 8, 10, 8, 0),
            arrival_airport="NRT",
            arrival_utc_date_time=datetime(2026, 8, 10, 10, 0),
        ),
    ],
    flight_assignments=[
        FlightAssignmentModel(id="A1", flight="OZ102", required_skill="Pilot"),
        FlightAssignmentModel(id="A2", flight="OZ102", required_skill="Flight attendant"),
    ],
)

result = fc.solve(problem, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_soft)
for a in result.flight_assignments:
    print(a.id, a.flight, a.employee)
```

- 시각은 UTC 다: naive `datetime` 은 UTC 로 해석하고, tz-aware 는 UTC 로
  변환한다. 불가일(`unavailable_days`)은 `date` 리스트.
- `pinned=True` 인 좌석은 배정이 고정된다 (점수엔 반영, 이동은 안 함).
- 엔진 객체(`fc.FlightCrewSchedule`, 인덱스 참조)를 직접 만들어
  `fc.Solver`/`fc.calculate_score` 를 쓰는 저수준 경로도 있다 — 이 경로는
  pydantic 이 필요 없다. 타입/필드는 `flight_crew/_core.pyi` 참고.
- 세부 설정은 `models.SolveConfig`(직렬화 가능) 또는 `fc.SolverConfig` 로:
  `fc.solve(problem, config=SolveConfig(time_limit_seconds=10, parallel_solver_count=4))`.

## 제약 (Java ConstraintProvider 그대로, HardSoftScore)

| 제약 | 레벨 | 벌점 |
|---|---|---|
| requiredSkill | hard | 스킬 없는 배정당 -100 |
| flightConflict | hard | 시간 겹치는 같은 승무원 배정 쌍당 -10 |
| transferBetweenTwoFlights | hard | 공항이 안 이어지는 연속 배정 쌍당 -1 |
| employeeUnavailability | hard | 불가일 배정당 -10 |
| firstAssignmentNotDepartingFromHome | soft | -1000 |
| lastAssignmentNotArrivingAtHome | soft | -1000 |

점수는 원본과 동일한 `HardSoftScore` 그대로다 (레벨 접기 없음).
