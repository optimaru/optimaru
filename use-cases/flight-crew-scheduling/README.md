# Flight Crew Scheduling (C++ / Python)

Timefold quickstart [flight-crew-scheduling](https://github.com/TimefoldAI/timefold-quickstarts)
을 이 레포의 C++ 엔진(`include/optimaru/`) 위에 포팅한 것이다. 항공편의 승무원
좌석(`FlightAssignment`)마다 승무원(`employee`)을 고르는 **BASIC 변수** 문제.

## 점수 매핑

Java 원본이 `HardSoftScore`(정수 2레벨)라 엔진의
`HardSoftScore(int64 hard, double soft)` 에 **그대로(identity)** 매핑된다.
레벨 접기 없음. 제약 가중치도 Java `ConstraintProvider` 그대로:

| 제약 | 레벨 | 벌점 |
|---|---|---|
| requiredSkill | hard | 배정당 -100 |
| flightConflict | hard | 겹치는 같은 승무원 배정 쌍당 -10 |
| transferBetweenTwoFlights | hard | 끊긴 연속 배정 쌍당 -1 |
| employeeUnavailability | hard | 불가일 배정당 -10 |
| firstAssignmentNotDepartingFromHome | soft | -1000 |
| lastAssignmentNotArrivingAtHome | soft | -1000 |

"연속" 의미론은 Java 스트림과 동일하다: 쌍 (a, b) 는 dep(a) < dep(b) 이고
같은 승무원의 다른 배정 c 가 dep(a) <= dep(c) < dep(b) 를 만족하지 않을 때
연속이다 — 출발시각이 동률인 배정이 있으면 그 시각의 배정은 쌍의 앞자리가
될 수 없고, 뒷자리 동률은 전부 센다. first/last 도 동률을 전부 센다.

## 빌드/실행

```bash
cd cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/fcs_test      # 제약별 손계산 + 랜덤 워크 + FULL_ASSERT + pinned
./build/fcs_demo 10   # Java DemoDataGenerator 규모 재현, 10초 solve

cd ../python
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

데모는 Java 데모와 같은 규모(공항 6, 5일, 항공편 14, 좌석 ~60, 승무원
~50)를 생성하고 10초 안에 feasible(hard 0)에 도달한다.

## 구성

- `cpp/include/fcs/domain.hpp` — 도메인 + `SolutionDescriptor` 특수화
  (pinned 확장 포함)
- `cpp/src/score_calculator.cpp` — 전체 재계산 (정답 기준)
- `cpp/src/incremental_calculator.cpp` — before/after 훅, 승무원 단위
  O(delta) 증분 계산
- `cpp/src/solver.cpp` — 엔진 래퍼 (LA + ALNS)
- `python/` — pybind11 패키지 `flight_crew` (pydantic v2 미러 포함,
  자세한 사용법은 `python/README.md`)

## 원본과의 차이 (caveat)

- Java `Airport.taxiTimeInMinutes` 는 원본에서도 어떤 제약/데모에도 쓰이지
  않는 필드라 생략했다.
- 데모 데이터는 Java 와 같은 생성 로직/규모지만 RNG 가 달라(std::mt19937_64
  vs java.util.Random) 인스턴스가 바이트 단위로 같지는 않다.
- `pinned` 는 이 포트의 확장이다 (원본 quickstart 에는 없음; es/st 포트와
  동일 패턴).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/flight-crew-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
from datetime import datetime, timedelta

import flight_crew as fc
from flight_crew.models import (
    AirportModel, EmployeeModel, FlightAssignmentModel, FlightCrewScheduleModel, FlightModel,
)

out = datetime(2026, 1, 5, 8)
flights = [
    FlightModel(flight_number="OUT", departure_airport="ICN", departure_utc_date_time=out,
                arrival_airport="NRT", arrival_utc_date_time=out + timedelta(hours=2)),
    FlightModel(flight_number="BACK", departure_airport="NRT",
                departure_utc_date_time=out + timedelta(hours=5),
                arrival_airport="ICN", arrival_utc_date_time=out + timedelta(hours=7)),
]
problem = FlightCrewScheduleModel(
    airports=[AirportModel(code="ICN"), AirportModel(code="NRT")],
    employees=[EmployeeModel(id="P1", name="Pilot 1", home_airport="ICN", skills=["Pilot"])],
    flights=flights,
    flight_assignments=[
        FlightAssignmentModel(id=f"A{i}", flight=f.flight_number, index_in_flight=1, required_skill="Pilot")
        for i, f in enumerate(flights)
    ],
)
result = fc.solve(problem, time_limit_seconds=5.0, random_seed=42)   # also accepts a dict
print(result.score_hard, result.score_soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`flight_crew.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/flight-crew-scheduling/cpp -B build/flight-crew-scheduling && cmake --build build/flight-crew-scheduling -j
ctest --test-dir build/flight-crew-scheduling
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/fcs/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/fcs/constraints/` (currently `employee_unavailability`, `first_assignment_not_departing_from_home`, `flight_conflict`, `last_assignment_not_arriving_at_home`, `required_skill`, `transfer_between_two_flights`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/fcs/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `fcs_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/flight_crew/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/flight-crew-scheduling/cpsat`](../../benchmarks/flight-crew-scheduling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/flight-crew-scheduling`](../../benchmarks/flight-crew-scheduling) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.
