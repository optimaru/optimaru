"""제약 DTO / 카탈로그: 런타임 조립이 제약 클래스와 어긋나지 않는가.

DTO 는 코드 생성물이 아니라 `registry.CONSTRAINT_TYPES` 에서 그 자리에서
만들어진다. 그래서 여기 검증은 "생성물이 최신인가" 가 아니라 "제약 클래스와
DTO 가 같은 것을 말하는가" 다.
"""

from dataclasses import fields

import pytest
from helpers import NO_WEIGHTS, course, make_problem, slot, teacher
from pydantic import TypeAdapter, ValidationError

from teacher_scheduling.api import CONSTRAINT_DTOS, ConstraintDto, catalog, to_constraints
from teacher_scheduling.solver.config import SolverConfig
from teacher_scheduling.solver.registry import (
    CONSTRAINT_TYPES,
    TeacherGap,
    WeeklyPeriodCounts,
    default_constraints,
    merge,
)
from teacher_scheduling.solver.solver import solve

_LIST = TypeAdapter(list[ConstraintDto])


# ---------------------------------------------------------------- DTO <-> 제약


def test_every_constraint_type_has_a_dto():
    assert len(CONSTRAINT_DTOS) == len(CONSTRAINT_TYPES)


@pytest.mark.parametrize("constraint_type", CONSTRAINT_TYPES, ids=lambda t: t.name)
def test_dto_fields_match_the_constraint_fields(constraint_type):
    """제약에 파라미터를 추가하면 DTO 가 그 자리에서 따라온다 — 손댈 곳이 없다."""
    dto = next(d for d in CONSTRAINT_DTOS if d.__name__ == f"{constraint_type.__name__}Dto")

    expected = {f.name for f in fields(constraint_type)}
    assert set(dto.model_fields) - {"type"} == expected


@pytest.mark.parametrize("constraint", default_constraints(), ids=lambda c: c.name)
def test_default_constraints_round_trip_through_their_dto(constraint):
    payload = {"type": constraint.name, **{f.name: getattr(constraint, f.name) for f in fields(constraint)}}
    assert to_constraints(_LIST.validate_python([payload])) == (constraint,)


def test_parameters_are_optional_in_the_payload():
    """파라미터를 생략하면 제약 클래스의 기본값이 그대로 남는다."""
    (restored,) = to_constraints(_LIST.validate_python([{"type": "weekly_period_counts"}]))
    assert restored == WeeklyPeriodCounts()
    assert restored.hard is True


def test_unknown_constraint_name_is_rejected():
    with pytest.raises(ValidationError):
        _LIST.validate_python([{"type": "no_such_constraint"}])


def test_unknown_parameter_is_rejected():
    """오타 난 파라미터가 조용히 무시되면 안 된다."""
    with pytest.raises(ValidationError):
        _LIST.validate_python([{"type": "teacher_gap", "wieght": 3}])


def test_hard_only_constraint_takes_no_parameters():
    with pytest.raises(ValidationError):
        _LIST.validate_python([{"type": "parallel_groups", "hard": False}])


# ---------------------------------------------------------------- 카탈로그


def test_catalog_classifies_hard_soft_and_either():
    kinds = {spec.name: spec.kind for spec in catalog()}
    assert kinds["weekly_period_counts"] == "either", "H1 은 완화할 수 있다"
    assert kinds["parallel_groups"] == "hard"
    assert kinds["teacher_gap"] == "soft"


def test_catalog_covers_every_registered_constraint():
    assert {spec.name for spec in catalog()} == {c.name for c in CONSTRAINT_TYPES}
    assert all(spec.label for spec in catalog()), "서버/UI 가 쓸 라벨이 비면 안 된다"


def test_catalog_default_weight_matches_the_class_default():
    weights = {spec.name: spec.default_weight for spec in catalog()}
    assert weights["teacher_gap"] == TeacherGap().weight
    assert weights["parallel_groups"] == 0, "가중치가 없는 하드 제약은 0"


# ---------------------------------------------------------------- 엔드투엔드


def test_solve_driven_entirely_by_dtos():
    """API 로 받은 제약 목록만으로 푼다 — 서버가 하는 일 그대로."""
    problem = make_problem([course(periods_per_week=3, max_periods_per_day=3)], [teacher()], days=1, periods=2)
    config = SolverConfig(time_limit_seconds=5.0, num_workers=4, weights=NO_WEIGHTS)

    payload = [{"type": "weekly_period_counts", "hard": False, "weight": 100}]
    overrides = to_constraints(_LIST.validate_python(payload))

    timetable = solve(problem, config, constraints=merge(default_constraints(NO_WEIGHTS), *overrides))
    assert timetable.status.has_solution
    assert timetable.penalties["weekly_period_counts"] == 1
    assert timetable.objective_value == 100


def test_dto_overrides_only_the_named_constraint():
    """일부만 보내도 나머지 기본 제약은 그대로 살아 있다."""
    problem = make_problem(
        [course(periods_per_week=2, max_periods_per_day=2, fixed_slots=frozenset({slot(0, 1), slot(0, 3)}))],
        [teacher()],
        days=1,
        periods=3,
    )
    config = SolverConfig(time_limit_seconds=5.0, num_workers=4, weights=NO_WEIGHTS)

    overrides = to_constraints(_LIST.validate_python([{"type": "teacher_gap", "weight": 0}]))
    timetable = solve(problem, config, constraints=merge(default_constraints(NO_WEIGHTS), *overrides))

    assert timetable.status.has_solution
    # H2(고정 배정)가 살아 있어야 두 칸이 지정한 자리에 그대로 들어간다.
    assert {lesson.slot.period for lesson in timetable.lessons} == {1, 3}
