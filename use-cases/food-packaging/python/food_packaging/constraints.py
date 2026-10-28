"""제약 조립 스펙 — 비용 함수를 선언으로 고르는 API.

기본 경로(`fp.solve(schedule)`)는 다섯 제약을 기본 가중치로 전부 켠다.
이 모듈은 그 옆의 선택 경로다: 제약을 골라 담고, 가중치를 바꾸고,
하드/소프트를 뒤집은 목록을 `solve(schedule, constraints=...)` 에 넘긴다.

가장 짧은 사용법은 :func:`get_constraints` 다. 기본 목록을 그대로 돌려주니
"기본에서 조금 바꾸는" 선언이 한 줄로 끝난다::

    import food_packaging as fp
    from food_packaging.constraints import (
        MinimizeMakespan, get_constraints, max_end_date_time,
    )

    fp.solve(schedule, constraints=get_constraints())           # 기본 그대로
    fp.solve(schedule, constraints=get_constraints([            # 골라 담기
        max_end_date_time, MinimizeMakespan(weight=10),
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
`fp.list_constraints()` 로 확인할 수 있다. 모듈 상위 인스턴스 이름도 같은
snake_case 라 `list_constraints()` 가 찍는 문자열과 그대로 대응한다.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "MaxEndDateTime",
    "IdealEndDateTime",
    "MinimizeMakespan",
    "OperatorCleaningConflict",
    "MaximizeJobsAssigned",
    "max_end_date_time",
    "ideal_end_date_time",
    "minimize_makespan",
    "operator_cleaning_conflict",
    "maximize_jobs_assigned",
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


class MaxEndDateTime(Constraint):
    """작업이 놓쳐선 안 되는 마감을 넘긴 분. 기본: 하드, 가중치 1.

    미배정 작업은 여기서 침묵한다 — 그건 `maximize_jobs_assigned` 가
    값매김한다."""

    name = "max_end_date_time"


class IdealEndDateTime(Constraint):
    """작업이 이상적 완료일을 넘긴 분. 기본: 미디엄, 가중치 10^9.

    이상일을 놓치는 건 비용이고 `max_end_date_time` 을 놓치는 건 위반이다 —
    모양은 같은데 레벨이 다른 이유가 그것이다. 미디엄 레벨은 엔진의 소프트
    레벨에 `medium * kMediumUnit` 로 접힌다."""

    name = "ideal_end_date_time"


class MinimizeMakespan(Constraint):
    """각 라인이 바쁜 시간의 **제곱**. 기본: 소프트, 가중치 1.

    제곱이라 총합을 줄이는 것보다 라인 간 작업을 고르게 펴는 쪽으로 민다.
    빈 라인은 비용이 없다.

    제곱이 `exponent` 가 아니라 cost() 안에 있는 건 의도적이다 — exponent 는
    **가중된** 비용을 셰이핑하는 손잡이라, 제곱을 정의 쪽에 둬야 호출자가
    그 위에 셰이핑을 더 얹을 수 있다."""

    name = "minimize_makespan"


class OperatorCleaningConflict(Constraint):
    """한 작업자를 공유하는 라인들에서 세정 구간이 겹친 분. 기본: 하드, 가중치 1.

    사람 하나가 기계 둘을 동시에 닦을 수는 없다. 충돌하는 작업이 **다른**
    라인에 있으므로 글로벌 제약이다 (같은 라인의 두 작업은 세정 구간이
    엄격히 순차라 겹칠 수 없다)."""

    name = "operator_cleaning_conflict"


class MaximizeJobsAssigned(Constraint):
    """어느 포장 라인에도 올라가지 않은 작업의 생산 분. 기본: 미디엄, 가중치 10^9.

    긴 작업을 버리는 게 짧은 작업을 버리는 것보다 아프다. 작업은 어느
    리스트에도 없는 상태(구성 휴리스틱 전의 초기 상태)일 수 있어 "라인에
    없음" 이 조건이고, 그건 어떤 단일 유닛의 성질이 아니라 글로벌이다."""

    name = "maximize_jobs_assigned"


#: 기본 가중치로 만들어 둔 인스턴스. 골라 담을 때 그대로 참조하면 된다.
max_end_date_time = MaxEndDateTime()
ideal_end_date_time = IdealEndDateTime()
minimize_makespan = MinimizeMakespan()
operator_cleaning_conflict = OperatorCleaningConflict()
maximize_jobs_assigned = MaximizeJobsAssigned()

#: 기본 세트 — `fp.list_constraints()` 와 같은 순서 (unit 먼저, global 나중).
_DEFAULTS: list[Constraint] = [
    max_end_date_time,
    ideal_end_date_time,
    minimize_makespan,
    operator_cleaning_conflict,
    maximize_jobs_assigned,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """비용 함수를 돌려준다. 인자가 없으면 기본 다섯 제약 전부.

    `constraints` 를 주면 그 목록을 그대로 쓴다 — 목록에 있는 제약만
    채점된다. 기본에서 하나만 바꾸고 싶으면 기본 인스턴스를 섞어 쓴다::

        get_constraints([max_end_date_time, MinimizeMakespan(weight=10)])
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

    ``costs`` 는 `fp.constraint_costs(schedule)` 가 돌려준 것이다: 제약마다
    unit 별 **원본** cost (가중치도 exponent 도 레벨 반올림도 적용되기 전).
    한 번 받아두면 가중치를 바꿔가며 재채점 없이 계산할 수 있다::

        raw = fp.constraint_costs(schedule)
        score(raw)                     # 기본 세트
        score(raw, picked)             # 골라 담은 목록

    솔버도 탐색 알고리즘도 거치지 않는다. 계산 규칙은 엔진과 같다 —
    unit 마다 ``weight * shape(cost, exponent)`` 를 구하고, 하드는 unit
    기여분을 **각각 반올림**해서 더한다 (엔진의 applyRaw 와 같은 순서;
    합계를 뒤에 반올림하면 값이 달라진다).

    ``constraints`` 를 주면 그 목록의 제약만 채점된다 — `get_constraints`
    와 같은 의미론이다. 모르는 이름은 ValueError.

    돌려주는 값은 **엔진 레벨**의 (hard, soft) 다. 이 도메인은 medium 을
    엔진 soft 에 kMediumUnit (10^9) 배수로 접어 넣으므로, `solution.score` 의 분해된
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
