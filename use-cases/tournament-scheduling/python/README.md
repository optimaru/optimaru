# tournament_scheduling

Timefold quickstart [tournament-scheduling](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/tournament-scheduling)
(Java/Quarkus) 를 `include/optimaru/` 의 C++ 엔진 위에 포팅한 패키지다. 팀을 일자별 경기
슬롯(TeamAssignment)에 배정한다 — C++ 도메인/계산기는 `../cpp`, 이 패키지는
그 pybind11 바인딩(`tournament_scheduling._core`)과 pydantic 미러다.

## 제약 (Java ConstraintProvider 와 동일 의미론)

| 이름 | 레벨 | 내용 |
|------|------|------|
| oneAssignmentPerDatePerTeam | Hard | 같은 팀은 하루에 한 경기만 (같은 날 같은 팀 배정 쌍마다 -1) |
| unavailabilityPenalty | Hard | 불가일 (팀, 일) 에 배정이 하나라도 있으면 fact 당 -1 |
| fairAssignmentCountPerTeam | Medium | 팀별 배정 수를 공평하게 (LoadBalance 불공정도) |
| evenlyConfrontationCount | Soft | 팀 쌍별 대결 수를 고르게 (LoadBalance 불공정도) |

"불공정도(unfairness)" 는 Timefold `LoadBalance::unfairness` 그대로:
`sqrt(sum(load^2) - sum(load)^2 / n)` 을 유효숫자 6자리(HALF_EVEN)로 반올림.
`n` 은 load ≥ 1 인 항목 수다 (Java 원본에 complement 가 없어서 배정 없는
팀/대결 없는 쌍은 집계에 안 들어간다).

## 점수 매핑 (HardMediumSoftBigDecimalScore → HardSoftScore)

엔진 점수는 `HardSoftScore(hard int64, soft double)` 라 Java 의 3레벨을
2레벨로 접는다 (`cpp/include/ts/domain.hpp` 주석과 동일):

```
hard = Java hard 그대로                      (0 == feasible)
soft = -(medium 불공정도 * 1e6 + soft 불공정도)   # MEDIUM_SCALE = 1e6
```

medium/soft 불공정도가 유효숫자 6자리 십진수라 이 도메인의 인스턴스
규모에서는 medium 이 soft 를 항상 지배한다 (레벨 우선순위 보존).
예: 7팀 전원을 쓰는 최선의 데모 해가 `0hard/-1195232.56soft` == Java
레벨로 `0hard/-1.19523medium/-2.56soft`.

### 주의: 목적함수의 전역최적은 팀을 덜 쓴다

`n` 이 load ≥ 1 인 팀만 세기 때문에 팀을 통째로 비우면 medium 이 좋아진다.
데모(72배정 / 7팀)에서 7팀을 다 쓰면 medium 최소가 `sqrt(10/7)=1.19523`
이지만, 6팀 x 12배정이면 medium 이 **0** 이다. 솔버는 이 전역최적을 찾으므로
데모 결과는 `0hard/-1.54919soft` (medium 0, soft 1.54919) 이고 한 팀은
0경기로 남는다. 이는 계산기 버그가 아니라 이식된 Java 목적함수 그대로의
최적해다. "모든 팀이 뛴다" 가 도메인 요구라면 솔버가 아니라 제약을 고쳐야
한다 (complement 항 추가 또는 팀당 최소 배정 hard 제약).

## 사용

```bash
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

```python
import tournament_scheduling as ts
from tournament_scheduling.models import demo_data

schedule = demo_data()  # Java DemoDataGenerator 재현: 팀 7, 18일 x 4슬롯, 불가일 12
result = ts.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_soft)  # 0, -1.54919 (feasible, 6팀 x 12배정)
```

`solve` 는 pydantic 모델(`models.TournamentScheduleModel`)과 `_core`
객체 둘 다 받는다. 배정 고정은 `TeamAssignmentModel.pinned=True`
(Java `@PlanningPin`), 첫 feasible 에서 멈추려면
`stop_at_first_feasible=True`.

## C++ 데모/테스트

```bash
cd ../cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/ts_test    # 손계산 제약 케이스 + 랜덤 무브 3000 incremental==full + FULL_ASSERT solve
./build/ts_demo 10 # Java 데모 재현, 10초 solve, feasible(0hard) 도달 assert
```
