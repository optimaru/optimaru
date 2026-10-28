"""제약 조립 스펙 — 비용 함수를 선언으로 고르는 API.

기본 경로(`fcs.solve(schedule)`)는 여섯 제약을 provider 가중치로 전부 켠다.
이 모듈은 그 옆의 선택 경로다: 제약을 골라 담고, 가중치를 바꾸고,
하드/소프트를 뒤집은 목록을 `solve(schedule, constraints=...)` 에 넘긴다.

가장 짧은 사용법은 :func:`get_constraints` 다. 기본 목록을 그대로 돌려주니
"기본에서 조금 바꾸는" 선언이 한 줄로 끝난다::

    import flight_crew as fcs
    from flight_crew.constraints import (
        RequiredSkill, flight_conflict, get_constraints,
    )

    fcs.solve(schedule, constraints=get_constraints())          # 기본 그대로
    fcs.solve(schedule, constraints=get_constraints([           # 골라 담기
        flight_conflict, RequiredSkill(weight=500),
    ]))

모듈 상위에 제약마다 기본 인스턴스가 있으니 그중 몇 개만 골라 리스트로
넘겨도 된다. 목록에 넣은 제약만 채점된다 — 빼면 그 항은 점수에서 사라진다.

모든 스펙은 ``(weight=None, hard=None, exponent=1.0)`` 를 받는다:

- ``weight``   제약 비용의 선형 배수. ``None`` 은 컴포넌트 기본값.
- ``hard``     ``True``/``False`` 로 기본 레벨을 뒤집는다; ``None`` 은 유지.
- ``exponent`` 부호 보존 거듭제곱 셰이핑: ``weight * sign(c) * |c| **
               exponent``. 2.0 = 제곱이라 몰린 위반이 기하급수로 비싸진다.

이름(`name`)은 C++ 컴포넌트 이름을 snake_case 로 옮긴 것이다. 코어가
기계적으로 유도하니 손으로 관리하는 별칭 표는 없다 —
`fcs.list_constraints()` 로 확인할 수 있다. 모듈 상위 인스턴스 이름도 같은
snake_case 라 `list_constraints()` 가 찍는 문자열과 그대로 대응한다.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "RequiredSkill",
    "FlightConflict",
    "TransferBetweenTwoFlights",
    "EmployeeUnavailability",
    "FirstAssignmentNotDepartingFromHome",
    "LastAssignmentNotArrivingAtHome",
    "required_skill",
    "flight_conflict",
    "transfer_between_two_flights",
    "employee_unavailability",
    "first_assignment_not_departing_from_home",
    "last_assignment_not_arriving_at_home",
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


class RequiredSkill(Constraint):
    """좌석이 요구하는 스킬이 없는 승무원 배정. 기본: 하드, 가중치 100.

    provider 의 하드 규칙 중 가장 무겁다 — 그 좌석을 못 맡는 승무원은 어떤
    스케줄 불편보다 나쁘다."""

    name = "required_skill"


class FlightConflict(Constraint):
    """같은 승무원의 배정 중 시간이 겹치는 쌍의 수. 기본: 하드, 가중치 10.

    한 여정 **안**의 쌍을 그냥 더한 값이라, 배정 하나가 들고 나는 정확한
    델타는 나머지 멤버와의 겹침 수다."""

    name = "flight_conflict"


class TransferBetweenTwoFlights(Constraint):
    """도착 공항과 다음 출발 공항이 다른 경우. 기본: 하드, 가중치 1.

    누군가는 그 사이를 비행기나 차로 옮겨줘야 한다. "다음" 의 정의는 Java
    스트림을 그대로 따른다 — 출발 시각이 같은 배정은 쌍을 시작하지 않고,
    뒤쪽의 동률은 모두 센다."""

    name = "transfer_between_two_flights"


class EmployeeUnavailability(Constraint):
    """승무원이 불가한 날짜에 걸친 배정. 기본: 하드, 가중치 10.

    출발일부터 도착일까지(양끝 포함) 비행이 걸치는 달력 날짜 중 하나라도
    불가하면 잡힌다."""

    name = "employee_unavailability"


class FirstAssignmentNotDepartingFromHome(Constraint):
    """첫 배정이 홈 공항에서 출발하지 않는 경우. 기본: 소프트, 가중치 1000.

    자기 시간을 써서 거기까지 가야 한다는 뜻이다. 가장 이른 출발 시각의
    동률은 모두 센다."""

    name = "first_assignment_not_departing_from_home"


class LastAssignmentNotArrivingAtHome(Constraint):
    """마지막 배정이 홈 공항에 도착하지 않는 경우. 기본: 소프트, 가중치 1000.

    로스터가 끝났는데 집에 못 돌아간 상태로 남는다. 가장 늦은 출발 시각의
    동률은 모두 센다."""

    name = "last_assignment_not_arriving_at_home"


#: 기본 가중치로 만들어 둔 인스턴스. 골라 담을 때 그대로 참조하면 된다.
required_skill = RequiredSkill()
flight_conflict = FlightConflict()
transfer_between_two_flights = TransferBetweenTwoFlights()
employee_unavailability = EmployeeUnavailability()
first_assignment_not_departing_from_home = FirstAssignmentNotDepartingFromHome()
last_assignment_not_arriving_at_home = LastAssignmentNotArrivingAtHome()

#: 기본 세트 — `fcs.list_constraints()` 와 같은 순서.
_DEFAULTS: list[Constraint] = [
    required_skill,
    flight_conflict,
    transfer_between_two_flights,
    employee_unavailability,
    first_assignment_not_departing_from_home,
    last_assignment_not_arriving_at_home,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """비용 함수를 돌려준다. 인자가 없으면 기본 여섯 제약 전부.

    `constraints` 를 주면 그 목록을 그대로 쓴다 — 목록에 있는 제약만
    채점된다. 기본에서 하나만 바꾸고 싶으면 기본 인스턴스를 섞어 쓴다::

        get_constraints([flight_conflict, RequiredSkill(weight=500)])
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

    ``costs`` 는 `fcs.constraint_costs(schedule)` 가 돌려준 것이다: 제약마다
    unit 별 **원본** cost (가중치도 exponent 도 레벨 반올림도 적용되기 전).
    한 번 받아두면 가중치를 바꿔가며 재채점 없이 계산할 수 있다::

        raw = fcs.constraint_costs(schedule)
        score(raw)                     # 기본 세트
        score(raw, picked)             # 골라 담은 목록

    솔버도 탐색 알고리즘도 거치지 않는다. 계산 규칙은 엔진과 같다 —
    unit 마다 ``weight * shape(cost, exponent)`` 를 구하고, 하드는 unit
    기여분을 **각각 반올림**해서 더한다 (엔진의 applyRaw 와 같은 순서;
    합계를 뒤에 반올림하면 값이 달라진다).

    ``constraints`` 를 주면 그 목록의 제약만 채점된다 — `get_constraints`
    와 같은 의미론이다. 모르는 이름은 ValueError.
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
