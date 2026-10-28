"""제약 조립 스펙 — 비용 함수를 선언으로 고르는 API.

기본 경로(`ms.solve(schedule)`)는 열두 제약을 provider 가중치로 전부 켠다.
이 모듈은 그 옆의 선택 경로다: 제약을 골라 담고, 가중치를 바꾸고,
하드/소프트를 뒤집은 목록을 `solve(schedule, constraints=...)` 에 넘긴다.

가장 짧은 사용법은 :func:`get_constraints` 다. 기본 목록을 그대로 돌려주니
"기본에서 조금 바꾸는" 선언이 한 줄로 끝난다::

    import meeting_scheduling as ms
    from meeting_scheduling.constraints import (
        OverlappingMeetings, get_constraints, room_conflict,
    )

    ms.solve(schedule, constraints=get_constraints())           # 기본 그대로
    ms.solve(schedule, constraints=get_constraints([            # 골라 담기
        room_conflict, OverlappingMeetings(weight=100),
    ]))

모듈 상위에 제약마다 기본 인스턴스가 있으니 그중 몇 개만 골라 리스트로
넘겨도 된다. 목록에 넣은 제약만 채점된다 — 빼면 그 항은 점수에서 사라진다.

모든 스펙은 ``(weight=None, hard=None, exponent=1.0)`` 를 받는다:

- ``weight``   제약 비용의 선형 배수. ``None`` 은 컴포넌트 기본값.
- ``hard``     ``True``/``False`` 로 기본 레벨을 뒤집는다; ``None`` 은 유지.
               **이 도메인의 미디엄 제약은 이 손잡이로 표현할 수 없다** —
               ``hard=False`` 는 소프트로 접어 미디엄 의미가 사라진다.
               미디엄 항은 ``None`` 으로 두는 게 맞다.
- ``exponent`` 부호 보존 거듭제곱 셰이핑: ``weight * sign(c) * |c| **
               exponent``. 2.0 = 제곱이라 몰린 위반이 기하급수로 비싸진다.

참석 충돌은 세 항으로 나뉜다: 둘 다 **필수**면 하드, 한쪽만 필수거나
둘 다 선호면 미디엄이다 — 불가능한 일정과 아쉬운 일정을 레벨로 가른다.

이름(`name`)은 C++ 컴포넌트 이름을 snake_case 로 옮긴 것이다. 코어가
기계적으로 유도하니 손으로 관리하는 별칭 표는 없다 —
`ms.list_constraints()` 로 확인할 수 있다. 모듈 상위 인스턴스 이름도 같은
snake_case 라 `list_constraints()` 가 찍는 문자열과 그대로 대응한다.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "AvoidOvertime",
    "RequiredRoomCapacity",
    "StartAndEndOnSameDay",
    "DoMeetingsAsSoonAsPossible",
    "AssignLargerRoomsFirst",
    "RoomConflict",
    "RequiredAttendanceConflict",
    "RequiredAndPreferredAttendanceConflict",
    "PreferredAttendanceConflict",
    "OneBreakBetweenConsecutiveMeetings",
    "OverlappingMeetings",
    "RoomStability",
    "avoid_overtime",
    "required_room_capacity",
    "start_and_end_on_same_day",
    "do_meetings_as_soon_as_possible",
    "assign_larger_rooms_first",
    "room_conflict",
    "required_attendance_conflict",
    "required_and_preferred_attendance_conflict",
    "preferred_attendance_conflict",
    "one_break_between_consecutive_meetings",
    "overlapping_meetings",
    "room_stability",
    "get_constraints",
]


@dataclass(frozen=True)
class Constraint:
    """제약 하나를 고르는 스펙: 가중치 / 레벨 / 셰이핑."""

    weight: float | None = None  # None = 컴포넌트 기본 가중치
    hard: bool | None = None  # None = 컴포넌트 기본 레벨
    exponent: float = 1.0
    name: ClassVar[str] = ""

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} must set a registry `name`")


class AvoidOvertime(Constraint):
    """달력 끝을 넘겨 끝나는 회의. 기본: 하드, 가중치 1.

    마지막 grain 인덱스에 TimeGrain 팩트가 없는 경우다. 벌점이 그 인덱스라
    더 많이 넘길수록 비싸다."""

    name = "avoid_overtime"


class RequiredRoomCapacity(Constraint):
    """배정된 방이 참석 인원을 감당하기에 모자란 수. 기본: 하드, 가중치 1."""

    name = "required_room_capacity"


class StartAndEndOnSameDay(Constraint):
    """마지막 grain 이 첫 grain 과 다른 날에 떨어지는 회의. 기본: 하드, 가중치 1.

    다른 날에 있는 마지막 인덱스의 TimeGrain 팩트마다 센다 (Java join 의
    모양). 달력을 아예 넘겨버린 경우는 0 이다 — 그건 `avoid_overtime` 이
    값매김한다."""

    name = "start_and_end_on_same_day"


class RoomConflict(Constraint):
    """두 회의가 같은 방을 같은 시간에 쓴 grain 수. 기본: 하드, 가중치 1."""

    name = "room_conflict"


class RequiredAttendanceConflict(Constraint):
    """**둘 다 필수** 참석인 사람의 겹친 grain 수. 기본: 하드, 가중치 1.

    두 회의에 동시에 반드시 들어가야 하는 사람이 있으면 일정 자체가 불가능해
    하드다. 한 회의가 같은 사람을 필수로 두 번 적은 경우도 여기에 접혀 있다."""

    name = "required_attendance_conflict"


class RequiredAndPreferredAttendanceConflict(Constraint):
    """한쪽은 필수, 다른 쪽은 선호인 사람의 겹친 grain 수. 기본: 미디엄, 가중치 MEDIUM_SCALE.

    양방향을 다 센다 — 간선이 정방향/역방향 가중치를 따로 들고 컴포넌트가
    그 둘을 더하는 이유다."""

    name = "required_and_preferred_attendance_conflict"


class PreferredAttendanceConflict(Constraint):
    """**둘 다 선호** 참석인 사람의 겹친 grain 수. 기본: 미디엄, 가중치 MEDIUM_SCALE.

    불가능한 게 아니라 아쉬운 것이라, 하드 규칙 아래이면서 모든 소프트 편의
    항 위인 미디엄에 앉는다."""

    name = "preferred_attendance_conflict"


class DoMeetingsAsSoonAsPossible(Constraint):
    """회의마다 자기 마지막 grain 인덱스. 기본: 소프트, 가중치 1.

    일정 전체를 달력 앞쪽으로 당긴다."""

    name = "do_meetings_as_soon_as_possible"


class AssignLargerRoomsFirst(Constraint):
    """배정된 방보다 엄격히 큰 방마다 그 정원 차이. 기본: 소프트, 가중치 1.

    큰 방부터 채워서 작은 방은 거기밖에 못 들어가는 회의에 남겨 둔다.
    방별 합계는 `ScoreCalculator::prepare` 가 미리 계산한다."""

    name = "assign_larger_rooms_first"


class OneBreakBetweenConsecutiveMeetings(Constraint):
    """사이에 grain 하나 없이 붙어 있는 두 회의. 기본: 소프트, 가중치 100.

    provider 의 소프트 항 중 가장 비싸다 — 틈 없는 회의로 가득한 하루가
    참석자가 실제로 불평하는 것이다."""

    name = "one_break_between_consecutive_meetings"


class OverlappingMeetings(Constraint):
    """아무 두 회의가 동시에 도는 grain 수. 기본: 소프트, 가중치 10.

    방도 참석자도 무관하다. 둘 다 들어가고 싶을 수 있는 회의가 겹치지
    않도록 달력을 펴 준다."""

    name = "overlapping_meetings"


class RoomStability(Constraint):
    """다음 회의가 두 grain 안에 시작하는데 방이 다른 공유 참석자. 기본: 소프트, 가중치 1.

    앞의 세 충돌 규칙과 달리 이건 겹침에 대한 게 아니다 — 회의가 많이
    겹치지 **않을** 때 정확히 발동한다 (짐 싸서 걸어가야 하니까)."""

    name = "room_stability"


#: 기본 가중치로 만들어 둔 인스턴스. 골라 담을 때 그대로 참조하면 된다.
avoid_overtime = AvoidOvertime()
required_room_capacity = RequiredRoomCapacity()
start_and_end_on_same_day = StartAndEndOnSameDay()
do_meetings_as_soon_as_possible = DoMeetingsAsSoonAsPossible()
assign_larger_rooms_first = AssignLargerRoomsFirst()
room_conflict = RoomConflict()
required_attendance_conflict = RequiredAttendanceConflict()
required_and_preferred_attendance_conflict = RequiredAndPreferredAttendanceConflict()
preferred_attendance_conflict = PreferredAttendanceConflict()
one_break_between_consecutive_meetings = OneBreakBetweenConsecutiveMeetings()
overlapping_meetings = OverlappingMeetings()
room_stability = RoomStability()

#: 기본 세트 — `ms.list_constraints()` 와 같은 순서 (unit 먼저, global 나중).
_DEFAULTS: list[Constraint] = [
    avoid_overtime,
    required_room_capacity,
    start_and_end_on_same_day,
    do_meetings_as_soon_as_possible,
    assign_larger_rooms_first,
    room_conflict,
    required_attendance_conflict,
    required_and_preferred_attendance_conflict,
    preferred_attendance_conflict,
    one_break_between_consecutive_meetings,
    overlapping_meetings,
    room_stability,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """비용 함수를 돌려준다. 인자가 없으면 기본 열두 제약 전부.

    `constraints` 를 주면 그 목록을 그대로 쓴다 — 목록에 있는 제약만
    채점된다. 기본에서 하나만 바꾸고 싶으면 기본 인스턴스를 섞어 쓴다::

        get_constraints([room_conflict, OverlappingMeetings(weight=100)])
    """
    if constraints is None:
        return list(_DEFAULTS)
    if not constraints:
        # C++ applyConstraints 와 같은 계약: 빈 목록은 "아무것도 채점하지
        # 않음" 이라 실수일 가능성이 높다. 필터 결과가 우연히 비었을 때
        # 조용히 "전부 채점" 으로 뒤집히면 안 된다.
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return list(constraints)


class Score(NamedTuple):
    """`score()` 가 돌려주는 (hard, soft). 둘 다 벌점이라 0 이하다."""

    hard: int
    soft: float


def _shape(cost: float, exponent: float) -> float:
    """부호 보존 거듭제곱. 보상(음수 cost)이 벌점으로 뒤집히지 않는다."""
    if exponent == 1.0 or cost == 0.0:
        return cost
    return -((-cost) ** exponent) if cost < 0.0 else cost**exponent


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """제약 원본 cost 에 스펙을 적용해 점수를 낸다 — 순수 파이썬.

    ``costs`` 는 `ms.constraint_costs(schedule)` 가 돌려준 것이다: 제약마다
    unit 별 **원본** cost (가중치도 exponent 도 레벨 반올림도 적용되기 전).
    한 번 받아두면 가중치를 바꿔가며 재채점 없이 계산할 수 있다::

        raw = ms.constraint_costs(schedule)
        score(raw)                     # 기본 세트
        score(raw, picked)             # 골라 담은 목록

    솔버도 탐색 알고리즘도 거치지 않는다. 계산 규칙은 엔진과 같다 —
    unit 마다 ``weight * shape(cost, exponent)`` 를 구하고, 하드는 unit
    기여분을 **각각 반올림**해서 더한다 (엔진의 applyRaw 와 같은 순서;
    합계를 뒤에 반올림하면 값이 달라진다).

    ``constraints`` 를 주면 그 목록의 제약만 채점된다 — `get_constraints`
    와 같은 의미론이다. 모르는 이름은 ValueError.

    돌려주는 값은 **엔진 레벨**의 (hard, soft) 다. 이 도메인은 medium 을
    엔진 soft 에 MEDIUM_SCALE 배수로 접어 넣으므로, `solution.score` 의 분해된
    레벨과 견주려면 같은 방식으로 접어서 비교해야 한다.
    """
    hard = 0
    soft = 0.0
    for spec in get_constraints(constraints):
        entry = costs.get(spec.name)
        if entry is None:
            raise ValueError(
                f"unknown constraint {spec.name!r}; available: " + ", ".join(repr(name) for name in sorted(costs))
            )
        weight = entry["weight"] if spec.weight is None else spec.weight
        is_hard = entry["hard"] if spec.hard is None else spec.hard
        exponent = spec.exponent
        for cost in entry["costs"]:
            value = weight * _shape(cost, exponent)
            if is_hard:
                # 엔진과 같이 unit 기여분마다 반올림한다 (llround 와 같은
                # half-away-from-zero; 파이썬 round 는 banker's rounding).
                hard += int(math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5))
            else:
                soft += value
    return Score(-hard, -soft if soft != 0.0 else 0.0)
