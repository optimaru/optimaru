"""제약 조립 스펙 — 비용 함수를 선언으로 고르는 API.

기본 경로(`sls.solve(schedule)`)는 열 제약을 quickstart 가중치로 전부 켠다.
이 모듈은 그 옆의 선택 경로다: 제약을 골라 담고, 가중치를 바꾸고,
하드/소프트를 뒤집은 목록을 `solve(schedule, constraints=...)` 에 넘긴다.

가장 짧은 사용법은 :func:`get_constraints` 다. 기본 목록을 그대로 돌려주니
"기본에서 조금 바꾸는" 선언이 한 줄로 끝난다::

    import sports_league as sls
    from sports_league.constraints import (
        ClassicMatches, get_constraints, matches_on_same_day,
    )

    sls.solve(schedule, constraints=get_constraints())          # 기본 그대로
    sls.solve(schedule, constraints=get_constraints([           # 골라 담기
        matches_on_same_day, ClassicMatches(weight=10_000),
    ]))

모듈 상위에 제약마다 기본 인스턴스가 있으니 그중 몇 개만 골라 리스트로
넘겨도 된다. 목록에 넣은 제약만 채점된다 — 빼면 그 항은 점수에서 사라진다.

모든 스펙은 ``(weight=None, hard=None, exponent=1.0)`` 를 받는다:

- ``weight``   제약 비용의 선형 배수. ``None`` 은 컴포넌트 기본값.
- ``hard``     ``True``/``False`` 로 기본 레벨을 뒤집는다; ``None`` 은 유지.
- ``exponent`` 부호 보존 거듭제곱 셰이핑: ``weight * sign(c) * |c| **
               exponent``. 2.0 = 제곱이라 몰린 위반이 기하급수로 비싸진다.

이동거리 다섯 항(`*_hop`)은 모두 km 단위라 서로 더해도 뜻이 통한다.
`classic_matches` 의 1'000 은 그 km 들과 견주기 위한 값이다 — 더비를
토요일에 두려고 탐색이 먼 길을 돌 만큼 비싸다.

이름(`name`)은 C++ 컴포넌트 이름을 snake_case 로 옮긴 것이다. 코어가
기계적으로 유도하니 손으로 관리하는 별칭 표는 없다 —
`sls.list_constraints()` 로 확인할 수 있다. 모듈 상위 인스턴스 이름도 같은
snake_case 라 `list_constraints()` 가 찍는 문자열과 그대로 대응한다.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "StartToAwayHop",
    "AwayToEndHop",
    "ClassicMatches",
    "MatchesOnSameDay",
    "ConsecutiveHomeMatches",
    "ConsecutiveAwayMatches",
    "RepeatMatchOnNextDay",
    "HomeToAwayHop",
    "AwayToAwayHop",
    "AwayToHomeHop",
    "start_to_away_hop",
    "away_to_end_hop",
    "classic_matches",
    "matches_on_same_day",
    "consecutive_home_matches",
    "consecutive_away_matches",
    "repeat_match_on_next_day",
    "home_to_away_hop",
    "away_to_away_hop",
    "away_to_home_hop",
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


class MatchesOnSameDay(Constraint):
    """같은 라운드에서 팀을 하나라도 공유하는 경기 쌍의 수. 기본: 하드, 가중치 1.

    두 팀을 **모두** 공유하는 쌍(대진과 그 역대진)도 한 번만 센다."""

    name = "matches_on_same_day"


class ConsecutiveHomeMatches(Constraint):
    """연속 네 라운드 이상 홈에서 뛴 팀. 기본: 하드, 가중치 1.

    벌점은 연속 구간 길이 전체다. 한 라운드에 홈 경기가 여러 개여도 한 번만
    센다 — 구간을 만드는 건 그 라운드의 존재 자체다."""

    name = "consecutive_home_matches"


class ConsecutiveAwayMatches(Constraint):
    """`consecutive_home_matches` 의 원정 쪽 대칭. 기본: 하드, 가중치 1.

    팀이 일주일씩 길바닥에서 사는 걸 실제로 막는 건 이쪽이다."""

    name = "consecutive_away_matches"


class RepeatMatchOnNextDay(Constraint):
    """바로 다음 라운드에 역대진이 열리는 경기. 기본: 하드, 가중치 1.

    거기에 역대진이 몇 개 있든 경기당 최대 1점이다 (Java 의 ifExists)."""

    name = "repeat_match_on_next_day"


class StartToAwayHop(Constraint):
    """시즌 **첫** 라운드 경기에 원정팀이 자기 홈에서 이동한 km. 기본: 소프트, 가중치 1."""

    name = "start_to_away_hop"


class HomeToAwayHop(Constraint):
    """홈 다음 라운드에 원정을 가는 팀의 이동 km. 기본: 소프트, 가중치 1.

    (라운드 i 의 m1, i+1 의 m2)이고 m2.away == m1.home 인 쌍에
    dist(m1.home, m2.home) 이 붙는다."""

    name = "home_to_away_hop"


class AwayToAwayHop(Constraint):
    """연속 두 라운드를 원정으로 도는 팀이 한 홈에서 다음 홈으로 간 km.
    기본: 소프트, 가중치 1."""

    name = "away_to_away_hop"


class AwayToHomeHop(Constraint):
    """원정 다음 라운드에 홈을 치르는 팀이 집으로 돌아온 km. 기본: 소프트, 가중치 1.

    거리는 **앞선** 경기의 dist(m1.home, m1.away) 다 — m1 에만 의존하므로,
    뒤쪽 경기는 자기 거리가 아니라 앞 경기의 거리를 낸다."""

    name = "away_to_home_hop"


class AwayToEndHop(Constraint):
    """시즌 **마지막** 라운드 경기 뒤 원정팀이 집으로 돌아간 km.
    기본: 소프트, 가중치 1."""

    name = "away_to_end_hop"


class ClassicMatches(Constraint):
    """주말도 휴일도 아닌 라운드에 열린 더비. 기본: 소프트, 가중치 1000.

    km 단위 이동 항들에 견줘 충분히 비싸다 — 더비를 토요일에 두려고 탐색이
    먼 길을 돌 만큼."""

    name = "classic_matches"


#: 기본 가중치로 만들어 둔 인스턴스. 골라 담을 때 그대로 참조하면 된다.
start_to_away_hop = StartToAwayHop()
away_to_end_hop = AwayToEndHop()
classic_matches = ClassicMatches()
matches_on_same_day = MatchesOnSameDay()
consecutive_home_matches = ConsecutiveHomeMatches()
consecutive_away_matches = ConsecutiveAwayMatches()
repeat_match_on_next_day = RepeatMatchOnNextDay()
home_to_away_hop = HomeToAwayHop()
away_to_away_hop = AwayToAwayHop()
away_to_home_hop = AwayToHomeHop()

#: 기본 세트 — `sls.list_constraints()` 와 같은 순서 (unit 먼저, global 나중).
_DEFAULTS: list[Constraint] = [
    start_to_away_hop,
    away_to_end_hop,
    classic_matches,
    matches_on_same_day,
    consecutive_home_matches,
    consecutive_away_matches,
    repeat_match_on_next_day,
    home_to_away_hop,
    away_to_away_hop,
    away_to_home_hop,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """비용 함수를 돌려준다. 인자가 없으면 기본 열 제약 전부.

    `constraints` 를 주면 그 목록을 그대로 쓴다 — 목록에 있는 제약만
    채점된다. 기본에서 하나만 바꾸고 싶으면 기본 인스턴스를 섞어 쓴다::

        get_constraints([matches_on_same_day, ClassicMatches(weight=10_000)])
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

    ``costs`` 는 `sls.constraint_costs(schedule)` 가 돌려준 것이다: 제약마다
    unit 별 **원본** cost (가중치도 exponent 도 레벨 반올림도 적용되기 전).
    한 번 받아두면 가중치를 바꿔가며 재채점 없이 계산할 수 있다::

        raw = sls.constraint_costs(schedule)
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
