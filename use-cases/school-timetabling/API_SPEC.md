# School Timetabling canonical JSON 계약

이 레포가 정의하는 것은 **문제와 해의 직렬화 형태**다. 원본은
`benchmarks/school-timetabling/cpsat/teacher_scheduling/domain/` 의 pydantic 모델
(`TimetableProblem` / `Timetable`)이고, 이 문서가 그 JSON 형태를 고정한다.
`python/`(pybind11 래퍼)과 `bench/` 도 같은 형태를 주고받는다.

> HTTP API 계약(엔드포인트·요청/응답 봉투·저장 계층)은 이 문서의 범위가 아니다.

## 공통 타입

- **Weekday**: 정수. `0=월 1=화 2=수 3=목 4=금 5=토`
- **TimeSlot**: `{"day": <Weekday>, "period": <int, 1-base>}`
- pydantic `frozenset` 필드는 JSON 배열로 직렬화된다 (순서 비보장).

## TimetableProblem

```json
{
  "calendar": {"periods_per_day": {"0": 7, "1": 7, "2": 7, "3": 7, "4": 6}},
  "subjects": [
    {"id": "MATH", "name": "수학", "required_room_type": null,
     "preferred_periods": [1, 2, 3, 4], "avoided_periods": []},
    {"id": "SCI", "name": "과학", "required_room_type": "과학실",
     "preferred_periods": [], "avoided_periods": []},
    {"id": "PE", "name": "체육", "required_room_type": "체육관",
     "preferred_periods": [], "avoided_periods": [1]}
  ],
  "teachers": [
    {"id": "T01", "name": "김수학", "subject_ids": ["MATH"],
     "unavailable": [{"day": 0, "period": 1}],
     "max_periods_per_week": 20, "max_periods_per_day": 6,
     "max_consecutive_periods": 4, "preferred_free_days": [4],
     "homeroom_class_id": "C11"}
  ],
  "class_groups": [
    {"id": "C11", "name": "1학년 1반", "grade": 1, "unavailable": []}
  ],
  "rooms": [
    {"id": "R_SCI1", "name": "과학실 1", "room_type": "과학실"},
    {"id": "R_GYM", "name": "체육관", "room_type": "체육관"}
  ],
  "courses": [
    {"id": "C11-MATH", "class_group_id": "C11", "subject_id": "MATH",
     "teacher_ids": ["T01"], "periods_per_week": 4, "block_periods": 0,
     "max_periods_per_day": 1, "fixed_slots": [], "forbidden_slots": [],
     "parallel_group": null}
  ]
}
```

- `teacher_ids` 가 2개 이상이면 동시수업(팀티칭).
- `parallel_group` 이 같은 강좌끼리는 분반: 항상 같은 시간에 진행되고 학급 칸은 하나만 차지.
- `block_periods >= 2` 면 그 시수만큼 주 1회 연강으로 붙인다.
- `SchoolCalendar` 는 전교 공통이다. 학년마다 요일 시수가 다르면 캘린더에는 요일별
  *최대* 교시를 넣고, 그보다 짧은 학년은 남는 칸을 그 학년 학급의 `unavailable` 로 막는다.

## Timetable

```json
{
  "status": "FEASIBLE",
  "lessons": [
    {"course_id": "C11-MATH", "class_group_id": "C11", "subject_id": "MATH",
     "teacher_ids": ["T01"], "slot": {"day": 0, "period": 2}, "room_id": null}
  ],
  "objective_value": 42.0,
  "best_objective_bound": 40.0,
  "wall_time_seconds": 9.7,
  "penalties": {"teacher_gap": 1, "class_gap": 0},
  "notes": []
}
```

- `status`: `OPTIMAL | FEASIBLE | INFEASIBLE | MODEL_INVALID | UNKNOWN`
- `room_id`: 특별실이 필요한 수업에만 채워진다. 일반 수업은 `null` (자기 교실).
- `INFEASIBLE` 이면 `lessons` 는 빈 배열.
- 하드/소프트 점수라는 필드는 없다. 품질 지표는 `status` · `objective_value` ·
  `best_objective_bound` · `penalties` 다.

## 가중치 (`ObjectiveWeights`)

소프트 제약 6종의 가중치. `penalties` 는 같은 key 의 **원시 카운트**이고
`objective_value` 는 그 가중 합이다.

```json
{"teacher_gap": 20, "teacher_workday": 3, "teacher_free_day": 30,
 "class_gap": 10, "avoided_period": 5, "non_preferred_period": 1}
```

## 두 백엔드

`benchmarks/school-timetabling/cpsat`(OR-Tools CP-SAT)와 `python`(optimaru local search)은
제약 의미론이 동일하다 — 하드 H1~H10 / 소프트 S1~S6 전부와 `weights` 가 같은
방식으로 반영된다. 패리티는 `python/tests/test_parity.py` 가 게이트로 고정한다.

차이는 탐색 방식뿐이다. local search 는 최적성/비가능성을 증명하지 못해 `status`
가 `FEASIBLE | UNKNOWN` 이고 `best_objective_bound` 는 항상 `null` 이다.
