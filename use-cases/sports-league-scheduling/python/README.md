# sports_league — Sports League Scheduling (C++ 엔진 + pybind11)

Timefold quickstart [sports-league-scheduling](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/sports-league-scheduling)
의 C++ 포팅(`../cpp`, 네임스페이스 `sls`)을 파이썬에서 쓰는 패키지.
리그의 모든 경기(`Match`)에 라운드(`Round`)를 배정한다 — `Match.round`
가 planning variable (-1 = 미배정, 값은 `rounds` 리스트의 위치).

## 설치 / 테스트

```sh
uv venv
uv pip install -e . pytest pydantic
uv run pytest -q
```

## 사용

```python
import sports_league as sl

schedule = sl.demo_schedule()  # Java DemoDataGenerator 재현 (14팀/32라운드/182경기)
sl.solve(schedule, time_limit_seconds=10.0, random_seed=42, stop_at_first_feasible=True)
print(schedule.score)  # 예: 0hard/-218994soft (첫 feasible 즉시 종료값)
print(sl.compute_breakdown(schedule).away_to_away_hop)
```

- `solve(schedule, *, time_limit_seconds, random_seed, parallel_solver_count,
  stop_at_first_feasible, on_new_best, config)` — in place 로 풀고 그대로
  돌려준다. `config` 로 `SolverConfig` 나 `models.SolveConfig` 를 줄 수 있다.
- `calculate_score` / `compute_breakdown` — 10개 제약 전체의 full recompute
  와 제약별 집계.
- 직렬화는 `sports_league.models` 의 pydantic v2 미러
  (`LeagueScheduleModel.from_core / to_core`, `SolveConfig`).

## 점수 매핑

Java 원본이 이미 `HardSoftScore` 라 엔진의 `HardSoftScore(int64 hard,
double soft)` 로 항등 매핑된다: hard = 위반 수 합의 음수 (0 == feasible),
soft = 이동 거리(km) 합 + 클래식 벌점(경기당 1000)의 음수 (soft 는 항상
정수 값이며 엔진의 double 필드에 담긴다).

## 제약 (Java `SportsLeagueSchedulingConstraintProvider` 와 동일 의미론)

| # | 제약 | 레벨 | 내용 |
|---|------|------|------|
| 1 | matchesOnSameDay | hard | 같은 라운드 인덱스에서 팀을 공유하는 경기 쌍마다 -1 |
| 2 | 4+ consecutive home | hard | 홈 경기가 구별되는 라운드 인덱스 4개 이상을 연속으로 덮으면 런 길이만큼 -n (같은 인덱스 중복은 1로 셈) |
| 3 | 4+ consecutive away | hard | 원정도 동일 |
| 4 | repeatMatchOnNextDay | hard | 다음 라운드 인덱스에 리턴 매치가 있으면 경기마다 -1 |
| 5 | startToAwayHop | soft | 이전 라운드 인덱스가 없으면 -dist(away, home) |
| 6 | homeToAwayHop | soft | 연속 인덱스 쌍에서 m2.away == m1.home 이면 -dist(m1.home, m2.home) |
| 7 | awayToAwayHop | soft | m2.away == m1.away 이면 -dist(m1.home, m2.home) |
| 8 | awayToHomeHop | soft | m2.home == m1.away 이면 -dist(m1.home, m1.away) |
| 9 | awayToEndHop | soft | 다음 라운드 인덱스가 없으면 -dist(home, away) |
| 10 | classicMatches | soft | 주말/휴일이 아닌 라운드의 클래식 경기마다 -1000 |

라운드 인접성은 전부 `Round.index` 기준이라 인덱스가 희소하면 (예: 0,1,4)
그 간극은 "라운드 없음"으로 처리된다 (Java 의 `ifNotExists(Round)` 와 동일).

## 데모 데이터

`demo_schedule()` 은 Java `DemoDataGenerator` 를 재현한다: 남미 14팀 실측
거리 행렬, 32라운드, 모든 순서쌍 182경기, `java.util.Random(0)` LCG 를
그대로 구현한 5% 클래식 추첨(+대칭 폐포, 총 18경기). 유일한 차이는 주말
라운드 결정 — Java 는 `LocalDate.now()` 기준이지만 여기서는 결정론을 위해
round 0 을 수요일로 고정했다 (C++ 데모 `sls_demo` 와 동일).
