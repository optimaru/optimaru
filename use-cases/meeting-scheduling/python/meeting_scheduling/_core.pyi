"""`meeting_scheduling._core` 타입 스텁 — meeting-scheduling C++ 솔버.

컴파일된 모듈은 `use-cases/meeting-scheduling/cpp` 의 C++ 도메인 구현을
감싼다; 그 구현은 `include/optimaru` 의 범용 엔진을 임포트한다.

Java quickstart 의 제약 (HardMediumSoftScore) 과 점수 매핑:
  hard 1. roomConflict          같은 방 두 미팅의 겹침 그레인마다 -1
  hard 2. avoidOvertime         마지막 그레인 인덱스에 TimeGrain 이 없으면
                                -lastTimeGrainIndex
  hard 3. requiredAttendanceConflict  필수 참석자 공유 쌍의 겹침 그레인마다 -1
  hard 4. requiredRoomCapacity  필요 좌석(필수+선호) 이 방 수용량을 넘으면
                                -(부족분)
  hard 5. startAndEndOnSameDay  시작일과 종료일이 다르면 -1
  med  6. requiredAndPreferredAttendanceConflict  (필수, 선호) 쌍의 겹침
                                그레인마다 -1
  med  7. preferredAttendanceConflict  선호 참석자 공유 쌍의 겹침마다 -1
  soft 8. doMeetingsAsSoonAsPossible  -lastTimeGrainIndex (미팅마다)
  soft 9. oneBreakBetweenConsecutiveMeetings  붙은 미팅 쌍마다 -100
  soft 10. overlappingMeetings  서로 다른 미팅의 겹침 그레인마다 -10
  soft 11. assignLargerRoomsFirst  더 큰 방마다 -(수용량 차)
  soft 12. roomStability        참석자의 연속(간격 <= 2그레인) 미팅이 방을
                                옮기면 -1

엔진 점수는 HardSoftScore(hard int, soft float) 라 medium 을 soft 에
접는다: ``soft = -(medium 벌점 * MEDIUM_SCALE + raw soft 벌점)``
(MEDIUM_SCALE == 1e6; medium 이 soft 를 항상 지배한다).

Java 엔티티의 planning variable 두 개(startingTimeGrain, room)는 합성
인코딩 ``grain_room = time_grain_pos * room_count + room_index`` 로 하나의
BASIC 변수다 (-1 == 미배정). 미팅은 시작 그레인부터 duration_in_grains 개의
연속 그레인 인덱스를 점유하며, 모든 겹침 제약이 그 구간 전체로 계산된다.
"""

from collections.abc import Callable, Iterable, Sequence
from typing import Any

MEDIUM_SCALE: float
GRAIN_LENGTH_IN_MINUTES: int

class Score:
    """HardSoft 점수. `hard` 는 정수 위반 합(0 == feasible); `soft` 는
    접힌 medium/soft 목적식. 사전식 비교(hard 우선)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """hard == 0 이면 True."""

class Person:
    """problem fact: 참석자. 참조는 people 리스트 인덱스로 한다."""
    def __init__(self, id: str, full_name: str = "") -> None: ...
    id: str
    full_name: str

class TimeGrain:
    """problem fact: 15분 타임그레인. grain_index 가 duration/overlap 의
    단위이고, (day_of_year, starting_minute_of_day) 가 달력 위치다."""
    def __init__(
        self,
        id: str,
        grain_index: int,
        day_of_year: int,
        starting_minute_of_day: int,
    ) -> None: ...
    id: str
    grain_index: int
    day_of_year: int
    starting_minute_of_day: int

class Room:
    """problem fact: 회의실 (capacity 는 hard 4 / soft 11 에 쓰인다)."""
    def __init__(self, id: str, name: str = "", capacity: int = 0) -> None: ...
    id: str
    name: str
    capacity: int

class Meeting:
    """problem fact: 미팅. required/preferred_attendees 는 people 인덱스
    리스트 — 원소 하나가 Java Attendance fact 하나다."""
    def __init__(
        self,
        id: str,
        topic: str = "",
        duration_in_grains: int = 1,
        required_attendees: Sequence[int] = (),
        preferred_attendees: Sequence[int] = (),
        entire_group_meeting: bool = False,
    ) -> None: ...
    id: str
    topic: str
    duration_in_grains: int
    required_attendees: list[int]
    preferred_attendees: list[int]
    entire_group_meeting: bool
    def required_capacity(self) -> int:
        """필수 + 선호 참석자 수 (Java getRequiredCapacity)."""

class MeetingAssignment:
    """planning entity: 미팅 하나의 (시작 그레인, 방) 배정. `grain_room` 이
    합성 planning variable (-1 == 미배정); meeting 은 meetings 인덱스이며
    미팅과 배정은 1:1 이어야 한다."""
    def __init__(
        self,
        id: str,
        meeting: int,
        grain_room: int = -1,
        pinned: bool = False,
    ) -> None: ...
    id: str
    meeting: int
    grain_room: int
    """time_grain_pos * room_count + room_index; -1 == 미배정."""
    pinned: bool
    """True 면 grain_room 을 고정 (점수엔 반영되지만 절대 움직이지 않는다)."""

class MeetingSchedule:
    """planning solution: people + time_grains/rooms(값 범위) + meetings +
    meeting_assignments(entity). solve 는 배정의 `grain_room` 과 `score` 를
    in place 로 바꾼다.

    주의: `meeting_assignments` 등 리스트 속성 접근은 복사본을 돌려주므로,
    배정을 바꾸려면 `set_assignment` 를 쓴다. 구조(미팅/그레인/방)를 바꾼
    뒤에는 새 MeetingSchedule 을 만들어야 한다 (파생 테이블은 최초 채점
    때 한 번 준비된다)."""
    def __init__(
        self,
        people: Sequence[Person],
        time_grains: Sequence[TimeGrain],
        rooms: Sequence[Room],
        meetings: Sequence[Meeting],
        meeting_assignments: Sequence[MeetingAssignment],
    ) -> None: ...
    people: list[Person]
    time_grains: list[TimeGrain]
    rooms: list[Room]
    meetings: list[Meeting]
    meeting_assignments: list[MeetingAssignment]
    @property
    def score(self) -> Score: ...
    @property
    def prepared(self) -> bool: ...
    def time_grain_count(self) -> int: ...
    def room_count(self) -> int: ...
    def grain_room_count(self) -> int: ...
    def grain_room_of(self, time_grain_pos: int, room_index: int) -> int:
        """합성 값 인코딩: time_grain_pos * room_count + room_index."""

    def time_grain_pos_of(self, grain_room: int) -> int: ...
    def room_index_of(self, grain_room: int) -> int: ...
    def assignment(self) -> list[int]:
        """배정별 현재 grain_room (-1 == 미배정)."""

    def set_assignment(self, assignment: Sequence[int]) -> None:
        """planning variable 일괄 쓰기 (길이는 배정 수와 같아야 한다)."""

class Breakdown:
    """제약별 원시 위반 카운트 (가중치 곱하기 전; Java ConstraintProvider 가
    penalize 하는 양 그대로)."""
    @property
    def room_conflict_grains(self) -> int: ...
    @property
    def overtime_sum(self) -> int: ...
    @property
    def required_conflict_grains(self) -> int: ...
    @property
    def room_capacity_shortage(self) -> int: ...
    @property
    def start_end_day_violations(self) -> int: ...
    @property
    def required_preferred_conflict_grains(self) -> int: ...
    @property
    def preferred_conflict_grains(self) -> int: ...
    @property
    def last_grain_index_sum(self) -> int: ...
    @property
    def consecutive_meeting_pairs(self) -> int: ...
    @property
    def overlapping_meeting_grains(self) -> int: ...
    @property
    def larger_room_capacity_sum(self) -> int: ...
    @property
    def room_stability_violations(self) -> int: ...
    def hard_total(self) -> int: ...
    def medium_total(self) -> int: ...
    def soft_total(self) -> int:
        """soft 가중치 적용 합 (x1/x100/x10 반영)."""

def calculate_score(schedule: MeetingSchedule) -> Score:
    """전체 재계산 (schedule.score 에도 저장)."""

def compute_breakdown(schedule: MeetingSchedule) -> Breakdown:
    """제약별 전체 재계산 (원시 위반 카운트)."""

def demo_data() -> MeetingSchedule:
    """Java DemoDataGenerator 재현 (java.util.Random(0) 과 같은 추첨):
    사람 20, 타임그레인 160, 회의실 3, 미팅 24."""

class SolverConfig:
    """엔진 설정 (기본: 30초 예산, late acceptance + ALNS)."""
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = 자동: clamp(128 * 배정 수, 3200, 6400)."""
    full_assert: bool
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    ruin_recreate_polish_steps: int
    ruin_recreate_as_move_weight: float
    """destroy-repair 를 acceptor 판정을 받는 일반 무브로도 뽑을 스텝당 확률
    (0 이면 비활성; 기본 2e-4, 품질 벤치 튜닝값)."""
    ruin_recreate_as_move_max_ruin: int
    """as-move 형 destroy-repair 한 번이 ruin 하는 배정 수 상한 (기본 12)."""
    change_move_weight: float
    swap_move_weight: float
    day_fit_value_probability: float
    """change 무브가 회의를 하루 안에 담는 시작 그레인만 뽑을 확률
    (0 이면 비활성)."""
    best_score_limit: tuple[int, float]
    """bestScore >= (hard, soft) 도달 시 조기 종료; (0, -1e18) 이면 첫
    feasible 에서 멈춘다."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: MeetingSchedule) -> None:
        """in place 로 푼다: 배정의 grain_room 과 schedule.score 를 쓴다.
        pinned 배정은 유지된다."""

    def solve_with_constraints(self, schedule: MeetingSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """같은 solve 를 호출자가 고른 제약 세트로 채점한다
        (meeting_scheduling.constraints 참고). 준 목록이 기본 세트를 대체하고, None 이면
        기본 세트를 그대로 쓴다."""

def list_constraints() -> list[str]:
    """이 도메인이 채점하는 제약 이름들 (기본 조립 순서, snake_case).
    meeting_scheduling.constraints 가 쓰는 이름과 같다."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """제약별 unit 별 **원본** cost — 가중치/exponent/레벨 반올림 전.
    `meeting_scheduling.constraints.score()` 가 이걸 받아 점수를 낸다.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
