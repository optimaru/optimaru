# meeting_scheduling

Timefold quickstart [meeting-scheduling](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/meeting-scheduling)
의 C++ 포팅(`../cpp`, 범용 엔진은 `include/optimaru`)에 pybind11
바인딩을 얹은 Python 패키지.

미팅마다 시작 타임그레인(15분 단위)과 회의실을 배정한다. Java 엔티티의
planning variable 두 개(`startingTimeGrain`, `room`)는 합성 인코딩
`grain_room = time_grain_pos * room_count + room_index` 로 하나의 BASIC
변수가 되고(-1 == 미배정), 미팅은 시작 그레인부터 `duration_in_grains` 개의
연속 그레인을 점유한다 — 모든 겹침 제약이 그 구간 전체로 계산된다.

## 제약 (Java ConstraintProvider 와 동일 의미론)

| # | 레벨 | 제약 | 단위당 벌점 |
|---|------|------|------------|
| 1 | hard | roomConflict | 같은 방 겹침 그레인당 1 |
| 2 | hard | avoidOvertime | 마지막 그레인 인덱스가 존재하지 않으면 lastTimeGrainIndex |
| 3 | hard | requiredAttendanceConflict | 필수 참석자 공유 쌍의 겹침 그레인당 1 |
| 4 | hard | requiredRoomCapacity | 좌석 부족분 (필수+선호 − 수용량) |
| 5 | hard | startAndEndOnSameDay | 시작/종료일이 다르면 1 |
| 6 | medium | requiredAndPreferredAttendanceConflict | (필수, 선호) 쌍 겹침 그레인당 1 |
| 7 | medium | preferredAttendanceConflict | 선호 참석자 공유 쌍 겹침 그레인당 1 |
| 8 | soft | doMeetingsAsSoonAsPossible | lastTimeGrainIndex |
| 9 | soft | oneBreakBetweenConsecutiveMeetings | 붙은 미팅 쌍당 100 |
| 10 | soft | overlappingMeetings | 서로 다른 미팅 겹침 그레인당 10 |
| 11 | soft | assignLargerRoomsFirst | 더 큰 방마다 수용량 차 |
| 12 | soft | roomStability | 참석자의 연속(간격 ≤ 2그레인) 미팅이 방을 옮기면 1 |

### 점수 매핑 (HardMediumSoftScore → HardSoftScore)

엔진 점수는 `HardSoftScore(hard int64, soft double)` 라 Java 의 medium
레벨을 soft 에 접는다:

```
hard = Java hard 그대로 (0 == feasible)
soft = -(medium 벌점 × 1e6 + raw soft 벌점)     # MEDIUM_SCALE == 1e6
```

raw soft 벌점(그레인 인덱스 합, 100×연속 쌍, 10×겹침 그레인, 수용량 차,
방 이동 수)이 1e6 을 넘지 않는 한 레벨 우선순위가 보존된다 — 현실적인
인스턴스에서는 항상 그렇다.

## 사용

```python
import meeting_scheduling as ms

schedule = ms.demo_data()  # Java DemoDataGenerator 재현 (미팅 24, 그레인 160)
result = ms.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(result.score, ms.compute_breakdown(result).medium_total())

for a, v in zip(result.meeting_assignments, result.assignment()):
    pos, room = result.time_grain_pos_of(v), result.room_index_of(v)
    print(result.meetings[a.meeting].topic, result.time_grains[pos].id, result.rooms[room].name)
```

pydantic 미러(직렬화)는 `meeting_scheduling.models`, 제약별 위반 카운트는
`compute_breakdown` (Java ConstraintVerifier 테스트가 이 카운트로 이식돼
있다).

## 빌드 / 테스트

```sh
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## 원본과의 차이 (caveats)

- 미팅과 `MeetingAssignment` 는 1:1 이어야 한다 (quickstart 데모 모델;
  위반 시 `ValueError`).
- 합성 인코딩이라 "타임그레인만 배정되고 방은 null" 같은 부분 배정 상태가
  없다. Java 의 `forEachIncludingUnassigned` 기반 제약(예: room == null 일
  때의 requiredRoomCapacity)은 그 상태에서만 다르게 채점되며, 완전히 배정된
  해에서는 의미론이 정확히 일치한다. 이 포팅에서 미배정(-1) 엔티티는 아무
  점수도 내지 않는다 (엔진 계약).
- `demo_data()` 는 `java.util.Random(0)` 을 재현해 Java 데모와 같은 추첨을
  쓰지만, 달력 날짜만 dayOfYear 200..203 으로 고정했다 (Java 는 "내일"부터
  4일).

## 솔버 기본값과 품질 벤치 (2026-08-18)

기본 탐색 설정은 `cpp/bench/ms_quality_bench.cpp` 로 튜닝한 값이다
(모놀리식 인크리멘털 fast path, 10초/단일 스레드, 시드 6~12개 interleaved
A/B; 절대치는 Apple Silicon macOS 단일 머신 실측이라 머신마다 다르다).
목적함수는 `medium 벌점 × 1e6 + raw soft` (hard 0 전제, 낮을수록 좋다).

채택된 기본값 (전체 이력은 `cpp/include/ms/solver.hpp` 의 주석):

- `late_acceptance_size = 0` → 자동 `clamp(128 × 배정 수, 3200, 6400)`
- `day_fit_value_probability = 0.8` (change 무브의 하루-안-배치 그레인 유도)
- `ruin_recreate_as_move_weight = 2e-4`, `ruin_recreate_as_move_max_ruin = 12`
  — destroy-repair 를 acceptor 판정을 받는 일반 무브로도 뽑는다. 스텝당
  확률 2e-4 는 크기 무관 평평한 봉우리였다 (1e-4/3e-4/5e-4 모두 열세).
- 인크리멘털 계산기 델타 경로는 합/증분 2-패스 + 사전 디코딩 캐시로
  벡터화돼 있다 (점수 의미론 불변; moves/s 만 +25~35%).

2026-08-18 라운드의 시드 101..106 평균 (10초):

| 인스턴스 | 이전 기본값 | 현재 기본값 | Δ |
| --- | --- | --- | --- |
| demo (미팅 24, 셀 480) | 3,059 | 3,049 | −0.3% (중립) |
| scaled:48 (셀 960) | 33,681,990 | 31,015,587 | −7.9% |
| scaled:96 (셀 1920) | 311,564,460 | 274,069,113 | −12.0% |

측정-후-기각 (재시도 금지 목록도 `solver.hpp` 주석 참고): as-move ruin
6/24, LA 3200/12800 고정, polish 2000/15000, change/swap 0.85/0.15,
dayFit 0.9, 킥 ruin 축소 (0.05–0.15).
