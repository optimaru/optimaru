"""제약 조립 스펙 — 비용 함수를 선언으로 고르는 API.

기본 경로(`ts.solve(schedule)`)는 네 제약을 quickstart 가중치로 전부 켠다.
이 모듈은 그 옆의 선택 경로다: 제약을 골라 담고, 가중치를 바꾸고,
하드/소프트를 뒤집은 목록을 `solve(schedule, constraints=...)` 에 넘긴다.

가장 짧은 사용법은 :func:`get_constraints` 다. 기본 목록을 그대로 돌려주니
"기본에서 조금 바꾸는" 선언이 한 줄로 끝난다::

    import tournament_scheduling as ts
    from tournament_scheduling.constraints import (
        EvenlyConfrontationCount, get_constraints, one_assignment_per_date_per_team,
    )

    ts.solve(schedule, constraints=get_constraints())           # 기본 그대로
    ts.solve(schedule, constraints=get_constraints([            # 골라 담기
        one_assignment_per_date_per_team, EvenlyConfrontationCount(weight=10),
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

이름(`name`)은 C++ 컴포넌트 이름을 snake_case 로 옮긴 것이다. 코어가
기계적으로 유도하니 손으로 관리하는 별칭 표는 없다 —
`ts.list_constraints()` 로 확인할 수 있다. 모듈 상위 인스턴스 이름도 같은
snake_case 라 `list_constraints()` 가 찍는 문자열과 그대로 대응한다.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "OneAssignmentPerDatePerTeam",
    "UnavailabilityPenalty",
    "FairAssignmentCountPerTeam",
    "EvenlyConfrontationCount",
    "one_assignment_per_date_per_team",
    "unavailability_penalty",
    "fair_assignment_count_per_team",
    "evenly_confrontation_count",
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


class OneAssignmentPerDatePerTeam(Constraint):
    """한 팀이 같은 날 가진 배정 쌍 수 (팀·날짜 칸마다 C(c, 2)). 기본: 하드, 가중치 1."""

    name = "one_assignment_per_date_per_team"


class UnavailabilityPenalty(Constraint):
    """배정이 하나라도 놓인 (팀, 날짜) 불가 팩트 수. 기본: 하드, 가중치 1.

    배정별이 아니라 **팩트별**이다 — 같은 칸에 팩트 두 개면 2, 팩트 하나에
    배정 세 개면 그대로 1 (Java 의 ifExists 의미론).

    C++ 쪽 구조체 이름은 `UnavailabilityPenaltyConstraint` 지만 컴포넌트가
    들고 있는 이름은 `unavailabilityPenalty` 다 — 같은 네임스페이스의 도메인
    팩트가 이미 그 이름을 쓰고 있어서 붙은 접미사라 이름에는 안 들어간다."""

    name = "unavailability_penalty"


class FairAssignmentCountPerTeam(Constraint):
    """팀별 배정 수의 LoadBalance 불공정도. 기본: 미디엄, 가중치 MEDIUM_SCALE.

    배정을 실제로 가진 팀만 균형 계산에 들어간다 (Java 규칙에 여집합이 없다).
    통계가 유닛을 가로지르므로 글로벌 제약이다."""

    name = "fair_assignment_count_per_team"


class EvenlyConfrontationCount(Constraint):
    """팀 쌍별 맞대결 수의 LoadBalance 불공정도. 기본: 소프트, 가중치 1.

    같은 날 배정을 함께 가진 쌍마다 한 번씩 맞대결로 세고 (자기 자신과는
    세지 않는다), 맞대결이 한 번이라도 있는 쌍만 균형에 들어간다.
    통계 대상이 팀 **쌍**이라 어느 유닛도 소유하지 않는 글로벌 제약이다."""

    name = "evenly_confrontation_count"


#: 기본 가중치로 만들어 둔 인스턴스. 골라 담을 때 그대로 참조하면 된다.
one_assignment_per_date_per_team = OneAssignmentPerDatePerTeam()
unavailability_penalty = UnavailabilityPenalty()
fair_assignment_count_per_team = FairAssignmentCountPerTeam()
evenly_confrontation_count = EvenlyConfrontationCount()

#: 기본 세트 — `ts.list_constraints()` 와 같은 순서 (unit 먼저, global 나중).
_DEFAULTS: list[Constraint] = [
    one_assignment_per_date_per_team,
    unavailability_penalty,
    fair_assignment_count_per_team,
    evenly_confrontation_count,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """비용 함수를 돌려준다. 인자가 없으면 기본 네 제약 전부.

    `constraints` 를 주면 그 목록을 그대로 쓴다 — 목록에 있는 제약만
    채점된다. 기본에서 하나만 바꾸고 싶으면 기본 인스턴스를 섞어 쓴다::

        get_constraints([one_assignment_per_date_per_team, EvenlyConfrontationCount(weight=10)])
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

    ``costs`` 는 `ts.constraint_costs(schedule)` 가 돌려준 것이다: 제약마다
    unit 별 **원본** cost (가중치도 exponent 도 레벨 반올림도 적용되기 전).
    한 번 받아두면 가중치를 바꿔가며 재채점 없이 계산할 수 있다::

        raw = ts.constraint_costs(schedule)
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
