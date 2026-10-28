"""캐노니컬 제약 조립 스펙 — 플러그인 방식으로 비용 함수를 만드는 API.

기본 fast path(모놀리식 인크리멘털 계산기)는 H1~H10/S1~S6 전부를 하드코딩된
구성으로 계산한다. 이 모듈은 그 옆의 선택 경로다: 제약을 하나씩 골라 weight
를 바꾸고, 하드/소프트 레벨을 뒤집고, 일부를 빼는 조립을 선언한 뒤
``solve(problem, constraints=...)`` 또는 ``canon.DecomposedCalculator`` 에
넘긴다 .

가장 짧은 사용법은 :class:`Assembly` 다 — 기본값은 캐노니컬 15개 전부라
"기본에서 조금 바꾸는" 선언이 한 줄로 끝난다::

    from st_timetabling.constraints import Assembly

    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap", weight=40).drop("teacher_workday"),
    )

es 처럼 스펙 인스턴스 리스트로 처음부터 조립할 수도 있다::

    from st_timetabling.constraints import ClassConflict, TeacherConflict, TeacherGap

    constraints = [ClassConflict(), TeacherConflict(), TeacherGap(weight=40)]
    result = st.solve(problem, constraints=constraints)

모든 스펙은 ``(weight=None, hard=None, exponent=1.0)`` 를 받는다:

- ``weight``   제약 비용의 선형 배수. ``None`` 은 컴포넌트 기본값
               (하드 H* = 1, 소프트 S* = cpsat ``ObjectiveWeights`` 기본
               가중치 — solve() 경로에서는 ``weights`` 인자가 그 기본을
               대신한다).
- ``hard``     ``True``/``False`` 로 기본 하드/소프트 레벨을 뒤집는다
               (각 클래스 docstring 의 "기본: 하드/소프트" 참고);
               ``None`` 은 기본 유지.
- ``exponent`` 부호 보존 거듭제곱 셰이핑: ``weight * sign(c) * |c| **
               exponent``. 2.0 = 제곱 — 몰린 위반이 기하급수로 비싸진다.

스펙은 순수 데이터 홀더다; C++ 코어가 ``.name`` / ``.weight`` / ``.hard`` /
``.exponent`` 를 읽는다. H2(고정 시간)는 스펙이 없다 — pinned 레슨으로
엔진이 아예 움직이지 않아 채점 대상이 아니다.
"""

from dataclasses import dataclass
from typing import ClassVar

from ._core import canon

__all__ = [
    "CPSAT_ALIASES",
    "ENGINE_ONLY",
    "STRUCTURAL_IN_ENGINE",
    "Assembly",
    "AvoidedPeriod",
    "BlockAdjacency",
    "ClassConflict",
    "ClassGap",
    "Constraint",
    "CourseDailyLimit",
    "NonPreferredPeriod",
    "RoomCapacity",
    "SlotNotAllowed",
    "TeacherConflict",
    "TeacherConsecutiveLimit",
    "TeacherFreeDay",
    "TeacherGap",
    "TeacherLoadLimit",
    "TeacherWorkday",
    "UnassignedLesson",
    "default_weights",
    "get_constraints",
    "soft_specs",
    "list_constraints",
    "resolve_name",
]


def list_constraints() -> tuple[str, ...]:
    """조립에 쓸 수 있는 제약 이름 전부 (C++ 레지스트리와 같은 순서)."""
    return tuple(canon.CONSTRAINT_NAMES)


@dataclass(frozen=True)
class Constraint:
    """기본 스펙: 제약 선택 + weight / level / shaping 메타데이터."""

    weight: float | None = None  # None = 컴포넌트 기본 가중치 유지
    hard: bool | None = None  # None = 컴포넌트 기본 레벨 유지
    exponent: float = 1.0
    name: ClassVar[str] = ""
    #: 컴포넌트 기본 레벨/가중치. 이 둘이 목적함수의 원본이다 — 소프트 항의
    #: 가중치 목록을 따로 손으로 들고 있으면 제약을 추가할 때 반드시 갈라진다
    #: (`soft_specs()` / `default_weights()` 가 여기서 나온다).
    default_hard: ClassVar[bool] = True
    default_weight: ClassVar[float] = 1.0

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} 는 registry `name` 을 설정해야 한다")


class UnassignedLesson(Constraint):
    """H1: 미배정 수업당 1점. 기본: 하드, weight 1.

    `hard=False` 로 내리면 **부분 편성**이 된다 (cpsat 의
    `WeeklyPeriodCounts(hard=false)` 와 같은 계약): 탐색에 미배정 무브가 붙어
    "배정하지 않는다" 가 선택지가 되고, 못 채운 시수는 하드 위반이 아니라
    `penalties["weekly_period_counts"]` 로 보고된다. 그때 `weight` 가 미배정
    1시간의 값이므로 다른 소프트 항 합보다 크게 둔다 (cpsat 기본 10000) —
    작게 두면 공강 몇 개를 아끼려고 수업을 빼 버린다."""

    name = "unassigned_lesson"


class ClassConflict(Constraint):
    """H3: 같은 시간의 동일 학급 수업 쌍당 1점. 기본: 하드, weight 1."""

    name = "class_conflict"


class TeacherConflict(Constraint):
    """H4: 같은 시간의 수업 쌍이 공유하는 교사당 1점 (팀티칭 포함).
    기본: 하드, weight 1."""

    name = "teacher_conflict"


class SlotNotAllowed(Constraint):
    """H5: allowed_slot 테이블 밖 배정당 1점 (금지 시간 + 교사/학급 불가
    시간이 변환 단계에서 그 테이블로 접힌다). 기본: 하드, weight 1."""

    name = "slot_not_allowed"


class RoomCapacity(Constraint):
    """H6: (특별실 타입, 시간)별 max(0, 수요 - 보유 수)의 합.
    기본: 하드, weight 1."""

    name = "room_capacity"


class CourseDailyLimit(Constraint):
    """H7: (강좌, 요일)별 max(0, 배정 수 - 일일 상한)의 합.
    기본: 하드, weight 1."""

    name = "course_daily_limit"


class BlockAdjacency(Constraint):
    """H8: 연강 그룹의 같은 요일 연속 교시가 끊긴 만큼. 기본: 하드, weight 1."""

    name = "block_adjacency"


class TeacherLoadLimit(Constraint):
    """H9: 교사별 일일/주간 수업 교시 상한 초과의 합. 기본: 하드, weight 1."""

    name = "teacher_load_limit"


class TeacherConsecutiveLimit(Constraint):
    """H10: 교사별 연속 수업 상한 초과의 합. 기본: 하드, weight 1."""

    name = "teacher_consecutive_limit"


class TeacherGap(Constraint):
    """S1: 교사의 하루 안에 낀 공강 교시 수. 기본: 소프트, weight 20."""

    name = "teacher_gap"
    default_hard = False
    default_weight = 20


class TeacherWorkday(Constraint):
    """S2: 수업이 있는 (교사, 요일)당 1점. 기본: 소프트, weight 3."""

    name = "teacher_workday"
    default_hard = False
    default_weight = 3


class TeacherFreeDay(Constraint):
    """S3: 희망 공강일에 수업이 있는 (교사, 요일)당 1점.
    기본: 소프트, weight 30."""

    name = "teacher_free_day"
    default_hard = False
    default_weight = 30


class ClassGap(Constraint):
    """S4: 학급의 하루 안에 낀 공강 교시 수. 기본: 소프트, weight 10."""

    name = "class_gap"
    default_hard = False
    default_weight = 10


class AvoidedPeriod(Constraint):
    """S5: 기피 교시에 배정된 수업당 1점 (분반은 멤버 과목마다 계수).
    기본: 소프트, weight 5."""

    name = "avoided_period"
    default_hard = False
    default_weight = 5


class NonPreferredPeriod(Constraint):
    """S6: 선호 교시가 있는데 그 밖에 배정된 수업당 1점.
    기본: 소프트, weight 1."""

    name = "non_preferred_period"
    default_hard = False
    default_weight = 1


_SPEC_CLASSES: tuple[type[Constraint], ...] = (
    UnassignedLesson,
    ClassConflict,
    TeacherConflict,
    SlotNotAllowed,
    RoomCapacity,
    CourseDailyLimit,
    BlockAdjacency,
    TeacherLoadLimit,
    TeacherConsecutiveLimit,
    TeacherGap,
    TeacherWorkday,
    TeacherFreeDay,
    ClassGap,
    AvoidedPeriod,
    NonPreferredPeriod,
)

_SPEC_BY_NAME: dict[str, type[Constraint]] = {cls.name: cls for cls in _SPEC_CLASSES}

#: cpsat(`teacher_scheduling.solver.registry`) 이름 -> 엔진 이름.
#:
#: 같은 제약을 두 백엔드가 다른 이름으로 부르고 있었다. 방향이 이쪽인 이유는
#: cpsat 이 제약 의미론의 레퍼런스이기 때문이다 (패리티 테스트가 그걸 고정한다).
#: 덕분에 같은 조립 선언을 두 백엔드에 그대로 보낼 수 있다 — 성능 비교의 전제다.
#: `room_capacity` 와 소프트 6종은 이름이 이미 같아 여기 없다.
CPSAT_ALIASES: dict[str, str] = {
    "weekly_period_counts": "unassigned_lesson",
    "class_group_conflicts": "class_conflict",
    "teacher_conflicts": "teacher_conflict",
    "daily_course_limits": "course_daily_limit",
    "block_periods": "block_adjacency",
    "teacher_workload": "teacher_load_limit",
    "teacher_consecutive_limits": "teacher_consecutive_limit",
}

#: cpsat 에는 제약이지만 엔진에서는 변환 단계가 구조적으로 보장하는 것들.
#: 분반은 하나의 점유 단위로 병합되고(convert.py), 고정 배치는 pinned Lesson 이라
#: 엔진이 아예 움직이지 않는다. 끄거나 조절할 대상이 없으므로 조립에 넣지 않는다.
STRUCTURAL_IN_ENGINE: frozenset[str] = frozenset({"fixed_slots", "parallel_groups"})

#: 반대로 엔진에만 스펙이 있는 것. cpsat 은 불가능한 슬롯의 변수를 아예 만들지
#: 않아(variables.py) 채점 대상이 없다.
ENGINE_ONLY: frozenset[str] = frozenset({"slot_not_allowed"})


def soft_specs() -> tuple[type[Constraint], ...]:
    """기본이 소프트인 제약들 (레지스트리 순서). 목적함수의 항이 이것이다."""
    return tuple(cls for cls in _SPEC_CLASSES if not cls.default_hard)


def default_weights() -> dict[str, float]:
    """소프트 항 이름 -> 기본 가중치. 제약을 추가하면 여기가 저절로 따라온다."""
    return {cls.name: cls.default_weight for cls in soft_specs()}


def resolve_name(name: str) -> str:
    """cpsat 이름을 엔진 이름으로 바꾼다. 엔진 이름은 그대로 통과한다.

    엔진이 구조적으로 보장하는 제약(`STRUCTURAL_IN_ENGINE`)은 조립 대상이
    아니므로 ValueError 다 — 두 백엔드를 잇는 쪽에서 걸러야 한다.
    """
    if name in STRUCTURAL_IN_ENGINE:
        raise ValueError(
            f"{name!r} 은 엔진에서 변환 단계가 보장하므로 조립에 넣을 수 없다 "
            "(분반은 점유 단위 병합, 고정 배치는 pinned Lesson)"
        )
    return CPSAT_ALIASES.get(name, name)


class Assembly:
    """캐노니컬 제약 조립 빌더. 기본은 15개 전부 — 모놀리식 기본 경로와 같은
    구성 — 이라 "기본에서 조금 바꾸는" 선언이 짧다.

    ``add`` 는 이름(또는 스펙 인스턴스)으로 제약을 넣되 **같은 이름이 이미
    있으면 교체**한다 (오버라이드 의미론; 중복 합산이 필요하면
    ``canon.DecomposedCalculator.add`` 를 직접 쓴다). ``drop`` 은 이름의
    제약을 뺀다. 둘 다 self 를 돌려줘 체이닝이 된다::

        Assembly().add("teacher_gap", weight=40).drop("teacher_workday")

    ``Assembly(constraints)`` 로 스펙 인스턴스/이름의 iterable 을 주면 기본
    세트 대신 그 목록에서 시작한다 (es 의 리스트 조립과 동일). Assembly 는
    스펙의 iterable 이라 ``canon.DecomposedCalculator(assembly)`` 에 바로
    넘어간다.
    """

    def __init__(self, constraints=None):
        self._specs: list[Constraint] = []
        if constraints is None:
            self._specs = [cls() for cls in _SPEC_CLASSES]
        else:
            for constraint in constraints:
                self.add(constraint)

    def add(
        self,
        constraint: str | Constraint,
        *,
        weight: float | None = None,
        hard: bool | None = None,
        exponent: float = 1.0,
    ) -> "Assembly":
        """제약을 추가한다 (같은 이름이 이미 있으면 교체). 이름을 주면
        weight/hard/exponent 로 스펙을 만들고, 스펙 인스턴스를 주면 그대로
        쓴다 (인스턴스와 키워드 오버라이드를 섞으면 ValueError). 알 수 없는
        이름은 ValueError.

        cpsat 이름(`CPSAT_ALIASES`)도 받는다 — 같은 조립 선언을 두 백엔드에
        그대로 보낼 수 있게 하려는 것이다."""
        if isinstance(constraint, str):
            spec_class = _SPEC_BY_NAME.get(resolve_name(constraint))
            if spec_class is None:
                raise ValueError(f"알 수 없는 제약 이름: {constraint!r} (list_constraints() 참고)")
            spec = spec_class(weight=weight, hard=hard, exponent=exponent)
        elif isinstance(constraint, Constraint):
            if weight is not None or hard is not None or exponent != 1.0:
                raise ValueError("스펙 인스턴스에는 weight/hard/exponent 를 함께 줄 수 없다 (인스턴스에 담아라)")
            spec = constraint
        else:
            raise TypeError(f"제약 이름(str) 또는 Constraint 스펙이어야 한다: {constraint!r}")
        for i, existing in enumerate(self._specs):
            if existing.name == spec.name:
                self._specs[i] = spec
                return self
        self._specs.append(spec)
        return self

    def drop(self, name: str) -> "Assembly":
        """이름의 제약을 조립에서 뺀다. 조립에 없으면 ValueError (오타 방지).

        `add` 와 마찬가지로 cpsat 이름도 받는다."""
        resolved = resolve_name(name)
        remaining = [spec for spec in self._specs if spec.name != resolved]
        if len(remaining) == len(self._specs):
            raise ValueError(f"조립에 없는 제약이라 뺄 수 없다: {name!r}")
        self._specs = remaining
        return self

    def __iter__(self):
        return iter(self._specs)

    def __len__(self) -> int:
        return len(self._specs)

    def __contains__(self, name: str) -> bool:
        return any(spec.name == CPSAT_ALIASES.get(name, name) for spec in self._specs)

    def __repr__(self) -> str:
        return f"Assembly([{', '.join(spec.name for spec in self._specs)}])"


def get_constraints(
    constraints: list[Constraint] | None = None,
) -> list[Constraint]:
    """비용 함수를 스펙 목록으로 돌려준다. 인자가 없으면 기본 열다섯 제약 전부.

    레포의 다른 열다섯 use case 와 같은 진입점이라 "기본에서 조금 바꾸는"
    선언이 어디서나 같은 모양으로 읽힌다::

        from st_timetabling.constraints import TeacherGap, get_constraints

        canon.DecomposedCalculator(get_constraints())          # 기본 그대로
        canon.DecomposedCalculator(get_constraints([           # 골라 담기
            ClassConflict(), TeacherGap(weight=40),
        ]))

    `constraints` 를 주면 그 목록을 그대로 쓴다 — 목록에 있는 제약만
    채점된다. 기본 세트는 :class:`Assembly` 가 인자 없이 만드는 것과 같다;
    이름으로 넣고 빼는 체이닝이 필요하면 그쪽을 쓰면 된다.
    """
    if constraints is None:
        return [cls() for cls in _SPEC_CLASSES]
    if not constraints:
        # C++ applyConstraints 와 같은 계약: 빈 목록은 "아무것도 채점하지
        # 않음" 이라 실수일 가능성이 높다. 필터 결과가 우연히 비었을 때
        # 조용히 "전부 채점" 으로 뒤집히면 안 된다.
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return list(constraints)
