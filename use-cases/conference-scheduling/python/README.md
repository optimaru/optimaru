# conference_scheduling — Conference Scheduling (C++ 엔진 + pybind11)

Timefold quickstart [conference-scheduling](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/conference-scheduling)
의 C++ 포팅(`../cpp`, 네임스페이스 `cs`)을 파이썬에서 쓰는 패키지.
모든 발표(`Talk`)에 (시간대, 발표장)을 배정한다 — `Talk.slot_room` 이
planning variable (-1 = 미배정, 값은 `timeslot_index * room_count +
room_index` 합성 인코딩; school-timetabling 과 같은 패턴).

## 설치 / 테스트

```sh
uv venv
uv pip install -e . pytest pydantic
uv run pytest -q
```

## 사용

```python
import conference_scheduling as cs

schedule = cs.demo_conference()  # Java DemoDataGenerator 재현 (15 talks / 6 slots / 5 rooms)
cs.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(schedule.score)  # 예: 0hard/-54420soft
print(cs.compute_breakdown(schedule).content_conflict)
```

- `solve(schedule, *, time_limit_seconds, random_seed, parallel_solver_count,
  stop_at_first_feasible, on_new_best, config)` — in place 로 풀고 그대로
  돌려준다. `config` 로 `SolverConfig` 나 `models.SolveConfig` 를 줄 수 있다.
- `calculate_score` / `compute_breakdown` — 37개 제약 전체의 full recompute
  와 제약별 매치 유닛 집계 (`hard_total`/`soft_penalty_total`/
  `soft_reward_total` 이 가중치를 적용).
- 직렬화는 `conference_scheduling.models` 의 pydantic v2 미러
  (`ConferenceScheduleModel.from_core / to_core`, `SolveConfig`).

## 점수 매핑

Java 원본이 이미 `HardSoftScore` 라 엔진의 `HardSoftScore(int64 hard,
double soft)` 로 항등 매핑된다: hard = 가중 위반-분(minute) 합의 음수
(0 == feasible), soft = 보상(다양성) − 벌점(선호/충돌) 가중 합 (항상 정수
값이며 엔진의 double 필드에 담긴다).

## 제약 (Java `ConferenceSchedulingConstraintProvider` 전수, 16 hard + 20 soft)

hard: `roomUnavailableTimeslot`(100k/분) `roomConflict`(1k/겹침분)
`speakerUnavailableTimeslot`(100/분·화자) `speakerConflict`(10/겹침분·공유화자)
`talkPrerequisiteTalks`(10/합산분) `talkMutuallyExclusiveTalksTags`
`consecutiveTalksPause` `crowdControl` + 필수/금지 시간대·발표장 태그 8종
(talk/speaker × required/prohibited × timeslot/room, 1/태그·분)

soft: `themeTrackConflict` `themeTrackRoomStability` `sectorConflict`(각 -10)
`audienceTypeDiversity`(+1) `audienceTypeThemeTrackConflict`(-1)
`audienceLevelDiversity`(+1) `contentAudienceLevelFlowViolation`(-10)
`contentConflict`(-100) `languageDiversity`(+10) `sameDayTalks`(-10)
`popularTalks`(-10) + 선호/기피 태그 8종(각 -20) + `speakerMakespan`(-20)

### value range 대체 (유일한 의미론 차이)

Java 는 `@ValueRangeProvider` 로 Talk 의 값 범위를 TalkType 호환
시간대/발표장으로 제한한다. 이 엔진의 BASIC 변수는 전역 (timeslot × room)
그리드 하나라, 포팅은 합성 hard 제약 `talkTypeCompatibility`
(100k × max(분,1), 비호환 슬롯/룸 각각)로 같은 효과를 낸다 — Java 기준
feasible 한 해에서는 절대 발화하지 않으며, 탐색도
`compatible_value_probability` (기본 0.9) 로 호환 셀에 편향된다.

## 데모 데이터

`demo_conference()` 는 Java `DemoDataGenerator` 를 (java.util.Random(0) 의
태그 드로우까지) 그대로 재현한다: Lab 2 + Breakout 13 talks, 하루 6개
시간대, 발표장 5개, 화자 12명. 10초 solve 로 hard 0 에 도달한다.
