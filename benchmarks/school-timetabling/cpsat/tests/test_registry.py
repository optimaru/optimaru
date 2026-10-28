"""제약 카탈로그(플러그인) 조립: 기본 구성 동등성 + 커스텀 제약 주입 예제.

커스텀 제약은 validate.validate_timetable 의 검증 범위 밖이므로,
여기서는 결과 시간표를 직접 확인한다.
"""

from dataclasses import dataclass

import pytest
from helpers import NO_WEIGHTS, course, make_problem, slot, teacher
from ortools.sat.python import cp_model

from teacher_scheduling.domain.entities import ClassGroup
from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.domain.solution import SolveStatus
from teacher_scheduling.solver.config import ObjectiveWeights, SolverConfig
from teacher_scheduling.solver.registry import (
    Constraint,
    WeeklyPeriodCounts,
    default_constraints,
    merge,
)
from teacher_scheduling.solver.solver import solve
from teacher_scheduling.solver.variables import TimetableVariables
from teacher_scheduling.validate import validate_timetable

# ---------------------------------------------------------------- 커스텀 플러그인 예제


@dataclass(frozen=True)
class NoLateFriday:
    """커스텀 하드 예제: 지정 교사는 금요일 late_from 교시부터 수업 금지.

    벌점 식을 안 돌려주므로(None) 순수 하드 제약으로만 동작한다.
    """

    teacher_id: str
    late_from: int = 5
    name: str = "no_late_friday"

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> None:
        for (teacher_id, timeslot), busy in variables.teacher_busy.items():
            if teacher_id == self.teacher_id and timeslot.day == Weekday.FRI and timeslot.period >= self.late_from:
                model.add(busy == 0)


@dataclass(frozen=True)
class FirstPeriodLoad:
    """커스텀 소프트 예제: 1교시에 배정된 수업 총량을 벌점으로 센다."""

    name: str = "first_period_load"

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        first = [var for (_course_id, timeslot), var in variables.x.items() if timeslot.period == 1]
        return cp_model.LinearExpr.sum(first) if first else None


@dataclass(frozen=True)
class WeightedFirstPeriodLoad(FirstPeriodLoad):
    """가중치를 스스로 들고 있는 커스텀 소프트 예제."""

    weight: int = 1


def _solve(problem, *, weights=NO_WEIGHTS, constraints=None):
    config = SolverConfig(time_limit_seconds=5.0, num_workers=4, weights=weights)
    return solve(problem, config, constraints=constraints)


def _with(weights, *extra):
    """기본 구성에 커스텀 제약을 얹는다."""
    return (*default_constraints(weights), *extra)


def _friday_only_problem(periods_per_week: int):
    """월~목이 통째로 막혀 금요일(6교시)에만 배정할 수 있는 학급 하나짜리 문제."""
    unavailable = frozenset(slot(day, period) for day in range(4) for period in range(1, 7))
    return make_problem(
        [course(periods_per_week=periods_per_week, max_periods_per_day=periods_per_week)],
        [teacher()],
        class_groups=[ClassGroup(id="C1", name="C1", grade=1, unavailable=unavailable)],
        days=5,
        periods=6,
    )


# ---------------------------------------------------------------- 기본 레지스트리 동등성


def test_explicit_default_constraints_match_default_solve():
    """기본 구성을 명시적으로 주입해도 기본 solve 와 완전히 같은 결과."""
    problem = make_problem(
        [course(periods_per_week=2, max_periods_per_day=2, fixed_slots=frozenset({slot(0, 1), slot(0, 5)}))],
        [teacher()],
        days=1,
        periods=5,
    )
    config = SolverConfig(time_limit_seconds=5.0, num_workers=4)

    implicit = solve(problem, config)
    explicit = solve(problem, config, constraints=default_constraints(config.weights))

    assert explicit.status == implicit.status
    assert explicit.lessons == implicit.lessons
    assert explicit.penalties == implicit.penalties
    assert explicit.objective_value == implicit.objective_value


def test_examples_satisfy_the_protocol():
    """하드든 소프트든 같은 프로토콜 하나를 만족한다."""
    assert isinstance(NoLateFriday("T1"), Constraint)
    assert isinstance(FirstPeriodLoad(), Constraint)
    assert all(isinstance(constraint, Constraint) for constraint in default_constraints())


# ---------------------------------------------------------------- 커스텀 하드 주입


def test_custom_hard_blocks_late_friday_slots():
    """금요일에만 3시간을 넣어야 할 때, 커스텀 하드가 5교시 이후를 막는다."""
    problem = _friday_only_problem(periods_per_week=3)
    baseline = _solve(problem)
    assert baseline.status.has_solution  # 커스텀 없이는 6교시까지 자유롭게 쓴다

    timetable = _solve(problem, constraints=_with(NO_WEIGHTS, NoLateFriday("T1")))
    assert timetable.status.has_solution
    assert validate_timetable(problem, timetable) == []  # 기본 하드 10종은 여전히 지킨다
    assert all(lesson.slot.day == Weekday.FRI for lesson in timetable.lessons)
    assert all(lesson.slot.period < 5 for lesson in timetable.lessons)


def test_custom_hard_makes_overfull_friday_infeasible():
    """금요일 5시간 수업은 커스텀 하드(1~4교시만 허용) 아래에서는 불가능해진다."""
    problem = _friday_only_problem(periods_per_week=5)
    assert _solve(problem).status.has_solution  # 커스텀 없이는 가능

    infeasible = _solve(problem, constraints=_with(NO_WEIGHTS, NoLateFriday("T1")))
    assert infeasible.status == SolveStatus.INFEASIBLE
    assert infeasible.lessons == ()


# ---------------------------------------------------------------- 커스텀 소프트 주입


def test_custom_soft_steers_away_from_first_period():
    """1교시 벌점을 주면 수업이 2교시 이후로 밀리고, 벌점이 0 으로 보고된다."""
    problem = make_problem([course(periods_per_week=2, max_periods_per_day=2)], [teacher()], days=1, periods=3)
    weights = ObjectiveWeights(**dict.fromkeys(NO_WEIGHTS.model_dump(), 0), first_period_load=1)

    timetable = _solve(problem, weights=weights, constraints=_with(weights, FirstPeriodLoad()))
    assert timetable.status.has_solution
    assert {lesson.slot.period for lesson in timetable.lessons} == {2, 3}
    assert timetable.penalties["first_period_load"] == 0
    # 기본 항목도 그대로 보고된다 (셀 대상이 없는 항목은 원래부터 생략된다).
    assert {"teacher_gap", "teacher_workday", "class_gap"} <= set(timetable.penalties)


def test_custom_soft_counts_forced_first_period():
    """1교시에 고정된 수업은 커스텀 벌점 1 로 집계된다."""
    problem = make_problem(
        [course(periods_per_week=1, fixed_slots=frozenset({slot(0, 1)}))],
        [teacher()],
        days=1,
        periods=3,
    )
    weights = ObjectiveWeights(**dict.fromkeys(NO_WEIGHTS.model_dump(), 0), first_period_load=1)

    timetable = _solve(problem, weights=weights, constraints=_with(weights, FirstPeriodLoad()))
    assert timetable.status.has_solution
    assert timetable.penalties["first_period_load"] == 1
    assert timetable.objective_value == 1


def test_unspecified_custom_weight_defaults_to_one():
    """가중치를 지정하지 않은 커스텀 항목은 가중치 1 로 목적함수에 들어간다."""
    problem = make_problem([course(periods_per_week=2, max_periods_per_day=2)], [teacher()], days=1, periods=3)

    timetable = _solve(problem, constraints=_with(NO_WEIGHTS, FirstPeriodLoad()))
    assert timetable.status.has_solution
    assert {lesson.slot.period for lesson in timetable.lessons} == {2, 3}
    assert timetable.penalties["first_period_load"] == 0


# ---------------------------------------------------------------- 가중치 매핑


def test_weight_for_declared_extra_and_unknown_names():
    weights = ObjectiveWeights(first_period_load=2)
    assert weights.weight_for("teacher_gap") == 20  # 선언 필드는 기본값 그대로
    assert weights.weight_for("first_period_load") == 2  # extra 로 지정한 커스텀 항목
    assert weights.weight_for("unknown_term") == 1  # 미지정 커스텀 항목은 1


def test_invalid_extra_weight_rejected():
    with pytest.raises(ValueError, match="0 이상의 정수"):
        ObjectiveWeights(first_period_load=-1)
    with pytest.raises(ValueError, match="0 이상의 정수"):
        ObjectiveWeights(first_period_load="heavy")


def test_instance_weight_wins_over_objective_weights():
    """제약 인스턴스가 가중치를 들고 있으면 그게 원본이다."""
    problem = make_problem(
        [course(periods_per_week=1, fixed_slots=frozenset({slot(0, 1)}))], [teacher()], days=1, periods=3
    )
    weights = ObjectiveWeights(**dict.fromkeys(NO_WEIGHTS.model_dump(), 0), first_period_load=1)

    # ObjectiveWeights 는 1 이라고 하지만 인스턴스가 5 를 들고 있다.
    timetable = _solve(problem, weights=weights, constraints=_with(weights, WeightedFirstPeriodLoad(weight=5)))
    assert timetable.penalties["first_period_load"] == 1
    assert timetable.objective_value == 5


# ---------------------------------------------------------------- H1 완화 (하드 <-> 소프트)


def test_weekly_period_counts_can_be_relaxed_into_a_penalty():
    """같은 제약을 hard=False 로 주면 미배정을 허용하고 벌점으로 센다."""
    # 하루 2교시뿐인데 3시수 -> 하드로는 불가능, 완화하면 1시수 미배정
    problem = make_problem([course(periods_per_week=3, max_periods_per_day=3)], [teacher()], days=1, periods=2)

    assert _solve(problem).status == SolveStatus.INFEASIBLE

    relaxed = merge(default_constraints(NO_WEIGHTS), WeeklyPeriodCounts(hard=False, weight=100))
    timetable = _solve(problem, constraints=relaxed)
    assert timetable.status.has_solution
    assert len(timetable.lessons) == 2
    assert timetable.penalties["weekly_period_counts"] == 1
    assert timetable.objective_value == 100


def test_merge_replaces_in_place_and_appends_new_names():
    """merge 는 같은 이름을 제자리에서 갈아끼우고 새 이름만 뒤에 붙인다."""
    base = default_constraints()
    merged = merge(base, WeeklyPeriodCounts(hard=False), NoLateFriday("T1"))

    assert [c.name for c in merged] == [*[c.name for c in base], "no_late_friday"]
    assert merged[0] == WeeklyPeriodCounts(hard=False)
