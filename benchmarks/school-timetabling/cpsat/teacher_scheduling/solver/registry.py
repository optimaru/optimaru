"""제약 카탈로그 — 제약 하나가 클래스 하나인 플러그인 레지스트리.

클래스의 필드가 곧 그 제약의 파라미터이고, `name` 이 API 에서 쓰는 식별자다.
`solve()` 는 이 객체들의 목록 하나만 받는다 — 하드 목록과 소프트 목록을 따로
넘기지 않고, 제약이 스스로 `model.add()` 로 막을지(하드) 벌점 식을 돌려줄지
(소프트) 정한다. `build()` 가 None 을 돌려주면 하드로만 동작한 것이고,
식을 돌려주면 그 식이 `name` 이름의 벌점 항목이 된다.

    from teacher_scheduling.solver.registry import WeeklyPeriodCounts, default_constraints, merge

    # 못 채운 시수를 INFEASIBLE 대신 아주 비싼 벌점으로 (= solve_partial)
    timetable = solve(
        problem,
        config,
        constraints=merge(default_constraints(), WeeklyPeriodCounts(hard=False, weight=10_000)),
    )

가중치는 제약 인스턴스의 `weight` 필드가 원본이다. `weight` 가 없는 커스텀
제약은 `ObjectiveWeights.weight_for(name)` 로 찾고, 거기에도 없으면 1 로 본다.

카탈로그(`CONSTRAINT_TYPES` / `constraint_specs()`)는 서버가 "어떤 제약을 어떤
파라미터로 켤 수 있는가" 를 그대로 읽어 가는 단일 원본이다. 서버가 이름을
따로 관리하면 솔버와 갈라지므로, 목록을 손으로 베끼지 말고 여기서 읽는다.

주의: `validate.validate_timetable` / `compute_penalties` 는 기본 구성
(하드 H1~H10, 소프트 S1~S6) 기준으로만 검사한다. 제약을 완화하거나 커스텀
제약을 주입해 얻은 해에서 그 규칙이 지켜졌는지의 검증은 호출자 몫이다.
"""

from collections.abc import Iterable, Sequence
from dataclasses import dataclass, fields
from typing import Any, ClassVar, Protocol, runtime_checkable

from ortools.sat.python import cp_model

from teacher_scheduling.solver import constraints as hard_impl
from teacher_scheduling.solver import objective
from teacher_scheduling.solver.config import ObjectiveWeights
from teacher_scheduling.solver.variables import TimetableVariables

#: 미배정 1시간의 기본 벌점. 다른 소프트 항목 합보다 확실히 크게 잡는다.
UNASSIGNED_WEIGHT = 10_000


@runtime_checkable
class Constraint(Protocol):
    """제약 플러그인.

    `build()` 는 모델에 하드 제약을 걸고, 벌점으로 셀 것이 있으면 그 선형식을
    돌려준다. 돌려준 식은 `Timetable.penalties[name]` 으로 보고되고 `weight`
    가중치로 목적함수에 더해진다.
    """

    name: str

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None: ...


# ---------------------------------------------------------------- 하드 제약 (H1~H10)


@dataclass(frozen=True)
class WeeklyPeriodCounts:
    """H1: 강좌별 주당 시수.

    `hard=True` 면 정확히 맞추고, `hard=False` 면 '이하' 로 완화한 뒤 못 채운
    시수를 벌점으로 돌린다 — 컴시간의 [미배정 목록] 이 이 모드다. 완화 모드의
    `weight` 를 다른 소프트 항목 합보다 크게 두면 채울 수 있는 시수는 전부
    채우고 정말 불가능한 것만 미배정으로 남는다.
    """

    name: ClassVar[str] = "weekly_period_counts"
    label: ClassVar[str] = "주당 시수 충족"

    hard: bool = True
    weight: int = UNASSIGNED_WEIGHT

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        if self.hard:
            hard_impl._add_weekly_period_counts(model, variables)
            return None

        for course in variables.problem.courses:
            model.add(sum(variables.course_vars(course.id)) <= course.periods_per_week)
        # 분반 그룹은 대표 강좌 하나만 센다. H5 로 그룹 전원이 같은 시간에 묶여
        # 있어 미배정 시수도 항상 같은데, 전원을 세면 한 칸 못 채운 것에 그룹
        # 크기만큼 벌점이 붙어 분반이 일반 강좌보다 부당하게 우선된다.
        terms = [
            course.periods_per_week - sum(variables.course_vars(course.id))
            for course in variables.occupancy_representatives()
        ]
        return sum(terms) if terms else None


@dataclass(frozen=True)
class _HardOnly:
    """파라미터 없이 켜고 끄기만 하는 하드 제약의 공통 몸통."""

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> None:
        """벌점 식을 안 돌려주므로(None) 하드로만 동작한다."""
        self._impl(model, variables)

    @staticmethod
    def _impl(model: cp_model.CpModel, variables: TimetableVariables) -> None:  # pragma: no cover - 하위에서 지정
        raise NotImplementedError


@dataclass(frozen=True)
class FixedSlots(_HardOnly):
    """H2: 고정 시간 배정."""

    name: ClassVar[str] = "fixed_slots"
    label: ClassVar[str] = "고정 배치 / 금지 시간"
    _impl = staticmethod(hard_impl._add_fixed_slots)


@dataclass(frozen=True)
class ParallelGroups(_HardOnly):
    """H5: 분반 동시 진행."""

    name: ClassVar[str] = "parallel_groups"
    label: ClassVar[str] = "분반 동시 배치"
    _impl = staticmethod(hard_impl._add_parallel_groups)


@dataclass(frozen=True)
class ClassGroupConflicts(_HardOnly):
    """H3: 학급 충돌 금지."""

    name: ClassVar[str] = "class_group_conflicts"
    label: ClassVar[str] = "학급 중복 배정 금지"
    _impl = staticmethod(hard_impl._add_class_group_conflicts)


@dataclass(frozen=True)
class TeacherConflicts(_HardOnly):
    """H4: 교사 충돌 금지."""

    name: ClassVar[str] = "teacher_conflicts"
    label: ClassVar[str] = "교사 중복 배정 금지"
    _impl = staticmethod(hard_impl._add_teacher_conflicts)


@dataclass(frozen=True)
class RoomCapacity(_HardOnly):
    """H6: 특별실 수용량."""

    name: ClassVar[str] = "room_capacity"
    label: ClassVar[str] = "특별실 수용량"
    _impl = staticmethod(hard_impl._add_room_capacity)


@dataclass(frozen=True)
class DailyCourseLimits(_HardOnly):
    """H7: 강좌 일일 최대 시수."""

    name: ClassVar[str] = "daily_course_limits"
    label: ClassVar[str] = "강좌 일일 시수 상한"
    _impl = staticmethod(hard_impl._add_daily_course_limits)


@dataclass(frozen=True)
class BlockPeriods(_HardOnly):
    """H8: 블록타임(연강)."""

    name: ClassVar[str] = "block_periods"
    label: ClassVar[str] = "연강 배치"
    _impl = staticmethod(hard_impl._add_block_periods)


@dataclass(frozen=True)
class TeacherWorkload(_HardOnly):
    """H9: 교사 일일/주당 최대 시수."""

    name: ClassVar[str] = "teacher_workload"
    label: ClassVar[str] = "교사 시수 상한"
    _impl = staticmethod(hard_impl._add_teacher_workload)


@dataclass(frozen=True)
class TeacherConsecutiveLimits(_HardOnly):
    """H10: 교사 연속 수업 제한."""

    name: ClassVar[str] = "teacher_consecutive_limits"
    label: ClassVar[str] = "교사 연속 수업 제한"
    _impl = staticmethod(hard_impl._add_teacher_consecutive_limits)


# ---------------------------------------------------------------- 소프트 항목 (S1~S6)


@dataclass(frozen=True)
class TeacherGap:
    """S1: 교사 하루 일과 중간의 공강 수."""

    name: ClassVar[str] = "teacher_gap"
    label: ClassVar[str] = "교사 공강 최소화"

    weight: int = 20

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none([span.gap for span in variables.teacher_day_spans().values()])


@dataclass(frozen=True)
class TeacherWorkday:
    """S2: 교사 출근일 수."""

    name: ClassVar[str] = "teacher_workday"
    label: ClassVar[str] = "교사 출근일 최소화"

    weight: int = 3

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none([span.working for span in variables.teacher_day_spans().values()])


@dataclass(frozen=True)
class TeacherFreeDay:
    """S3: 희망 공강일에 수업이 잡힌 횟수."""

    name: ClassVar[str] = "teacher_free_day"
    label: ClassVar[str] = "교사 희망 공강일"

    weight: int = 30

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        spans = variables.teacher_day_spans()
        return objective._sum_or_none(objective._free_day_violations(variables, spans))


@dataclass(frozen=True)
class ClassGap:
    """S4: 학급 하루 일과 중간의 공강 수."""

    name: ClassVar[str] = "class_gap"
    label: ClassVar[str] = "학급 공강 최소화"

    weight: int = 10

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none([span.gap for span in variables.class_day_spans().values()])


@dataclass(frozen=True)
class AvoidedPeriod:
    """S5: 과목 기피 교시 배치 횟수."""

    name: ClassVar[str] = "avoided_period"
    label: ClassVar[str] = "기피 교시 회피"

    weight: int = 5

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none(objective._period_preference_vars(variables, avoided=True))


@dataclass(frozen=True)
class NonPreferredPeriod:
    """S6: 과목 선호 교시를 벗어난 배치 횟수."""

    name: ClassVar[str] = "non_preferred_period"
    label: ClassVar[str] = "선호 교시 우대"

    weight: int = 1

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none(objective._period_preference_vars(variables, avoided=False))


# ---------------------------------------------------------------- 카탈로그

#: 등록된 제약 전부. 순서가 곧 기본 조립 순서다(하드 H1~H10 -> 소프트 S1~S6).
#: 여기에 클래스를 추가하면 DTO(`teacher_scheduling.api`)와 서버 카탈로그에
#: 자동으로 따라 붙는다.
CONSTRAINT_TYPES: tuple[type[Constraint], ...] = (
    WeeklyPeriodCounts,  # H1
    FixedSlots,  # H2
    ParallelGroups,  # H5
    ClassGroupConflicts,  # H3
    TeacherConflicts,  # H4
    RoomCapacity,  # H6
    DailyCourseLimits,  # H7
    BlockPeriods,  # H8
    TeacherWorkload,  # H9
    TeacherConsecutiveLimits,  # H10
    TeacherGap,  # S1
    TeacherWorkday,  # S2
    TeacherFreeDay,  # S3
    ClassGap,  # S4
    AvoidedPeriod,  # S5
    NonPreferredPeriod,  # S6
)

CONSTRAINT_TYPE_BY_NAME: dict[str, type[Constraint]] = {c.name: c for c in CONSTRAINT_TYPES}


@dataclass(frozen=True)
class ConstraintParam:
    """제약이 받는 파라미터 하나 (= 데이터클래스 필드 하나)."""

    name: str
    type_name: str
    default: Any


@dataclass(frozen=True)
class ConstraintSpec:
    """카탈로그 한 줄. 서버가 "무엇을 어떻게 켤 수 있는가" 를 이걸로 그린다."""

    name: str
    label: str
    #: `"hard"` 는 끄거나 완화할 수 없는 제약, `"soft"` 는 가중치만 있는 벌점,
    #: `"either"` 는 `hard` 파라미터로 두 모드를 다 구현한 제약이다.
    kind: str
    params: tuple[ConstraintParam, ...]

    @property
    def default_weight(self) -> int:
        for param in self.params:
            if param.name == "weight":
                return param.default
        return 0


def _params_of(constraint_type: type[Constraint]) -> tuple[ConstraintParam, ...]:
    return tuple(
        ConstraintParam(name=f.name, type_name=getattr(f.type, "__name__", str(f.type)), default=f.default)
        for f in fields(constraint_type)
    )


def constraint_specs() -> tuple[ConstraintSpec, ...]:
    """등록된 제약의 이름/라벨/파라미터. 손으로 베낀 목록 대신 이걸 읽는다."""
    specs = []
    for constraint_type in CONSTRAINT_TYPES:
        params = _params_of(constraint_type)
        names = {param.name for param in params}
        if "hard" in names:
            kind = "either"
        elif "weight" in names:
            kind = "soft"
        else:
            kind = "hard"
        specs.append(ConstraintSpec(name=constraint_type.name, label=constraint_type.label, kind=kind, params=params))
    return tuple(specs)


def default_constraints(weights: ObjectiveWeights | None = None) -> tuple[Constraint, ...]:
    """기본 구성(하드 H1~H10 + 소프트 S1~S6)을 기본 파라미터로 조립한다.

    `weights` 를 주면 거기에 **실제로 들어 있는 이름만** 골라 `weight` 를 덮어쓴다
    — `ObjectiveWeights` 로만 가중치를 주던 기존 호출을 그대로 받기 위한 것이다.
    모르는 이름에 `weight_for` 의 기본값 1 이 끼어들면 안 되므로 존재 여부를 본다.
    """
    given = weights.model_dump() if weights is not None else {}
    built = []
    for constraint_type in CONSTRAINT_TYPES:
        kwargs: dict[str, Any] = {}
        name = constraint_type.name
        if name in given and any(param.name == "weight" for param in _params_of(constraint_type)):
            kwargs["weight"] = given[name]
        built.append(constraint_type(**kwargs))
    return tuple(built)


def merge(base: Sequence[Constraint], *overrides: Constraint) -> tuple[Constraint, ...]:
    """`base` 에서 같은 이름을 `overrides` 로 갈아끼우고, 새 이름은 뒤에 붙인다.

    조립 순서(= 변수 생성 순서)를 건드리지 않으려고 자리를 그대로 지킨다.
    """
    replacement = {constraint.name: constraint for constraint in overrides}
    merged = [replacement.pop(constraint.name, constraint) for constraint in base]
    merged.extend(constraint for constraint in overrides if constraint.name in replacement)
    return tuple(merged)


def weight_of(constraint: Constraint, weights: ObjectiveWeights) -> int:
    """제약의 가중치. 인스턴스의 `weight` 가 원본이고, 없으면 `weights` 에서 찾는다."""
    own = getattr(constraint, "weight", None)
    return weights.weight_for(constraint.name) if own is None else own


def build_all(
    model: cp_model.CpModel,
    variables: TimetableVariables,
    constraints: Iterable[Constraint],
) -> objective.PenaltyTerms:
    """제약들을 순서대로 조립하고, 벌점 식을 돌려준 것만 이름별로 모은다."""
    penalties: objective.PenaltyTerms = {}
    for constraint in constraints:
        expr = constraint.build(model, variables)
        if expr is not None:
            penalties[constraint.name] = expr
    return penalties


__all__ = [
    "CONSTRAINT_TYPES",
    "CONSTRAINT_TYPE_BY_NAME",
    "UNASSIGNED_WEIGHT",
    "AvoidedPeriod",
    "BlockPeriods",
    "ClassGap",
    "ClassGroupConflicts",
    "Constraint",
    "ConstraintParam",
    "ConstraintSpec",
    "DailyCourseLimits",
    "FixedSlots",
    "NonPreferredPeriod",
    "ParallelGroups",
    "RoomCapacity",
    "TeacherConflicts",
    "TeacherConsecutiveLimits",
    "TeacherFreeDay",
    "TeacherGap",
    "TeacherWorkday",
    "TeacherWorkload",
    "WeeklyPeriodCounts",
    "build_all",
    "constraint_specs",
    "default_constraints",
    "merge",
    "weight_of",
]
