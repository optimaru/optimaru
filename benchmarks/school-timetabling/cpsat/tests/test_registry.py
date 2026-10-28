"""Constraint catalog (plugins) assembly: default-configuration equivalence + custom-constraint examples.

Custom constraints are outside validate.validate_timetable's scope, so these
tests check the resulting timetable directly.
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

# ---------------------------------------------------------------- custom plugin examples


@dataclass(frozen=True)
class NoLateFriday:
    """Custom hard example: the given teacher has no lessons from period late_from on Fridays.

    Returns no penalty expression (None), so it acts purely as a hard constraint.
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
    """Custom soft example: counts the total lessons placed in period 1 as a penalty."""

    name: str = "first_period_load"

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        first = [var for (_course_id, timeslot), var in variables.x.items() if timeslot.period == 1]
        return cp_model.LinearExpr.sum(first) if first else None


@dataclass(frozen=True)
class WeightedFirstPeriodLoad(FirstPeriodLoad):
    """Custom soft example that carries its own weight."""

    weight: int = 1


def _solve(problem, *, weights=NO_WEIGHTS, constraints=None):
    config = SolverConfig(time_limit_seconds=5.0, num_workers=4, weights=weights)
    return solve(problem, config, constraints=constraints)


def _with(weights, *extra):
    """Puts custom constraints on top of the default configuration."""
    return (*default_constraints(weights), *extra)


def _friday_only_problem(periods_per_week: int):
    """A one-class problem with Mon–Thu fully blocked, so only Friday (6 periods) is available."""
    unavailable = frozenset(slot(day, period) for day in range(4) for period in range(1, 7))
    return make_problem(
        [course(periods_per_week=periods_per_week, max_periods_per_day=periods_per_week)],
        [teacher()],
        class_groups=[ClassGroup(id="C1", name="C1", grade=1, unavailable=unavailable)],
        days=5,
        periods=6,
    )


# ---------------------------------------------------------------- default registry equivalence


def test_explicit_default_constraints_match_default_solve():
    """Injecting the default configuration explicitly gives exactly the default solve result."""
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
    """Hard or soft, they satisfy the same single protocol."""
    assert isinstance(NoLateFriday("T1"), Constraint)
    assert isinstance(FirstPeriodLoad(), Constraint)
    assert all(isinstance(constraint, Constraint) for constraint in default_constraints())


# ---------------------------------------------------------------- custom hard injection


def test_custom_hard_blocks_late_friday_slots():
    """With 3 periods to place only on Friday, the custom hard constraint blocks period 5 and later."""
    problem = _friday_only_problem(periods_per_week=3)
    baseline = _solve(problem)
    assert baseline.status.has_solution  # without it, periods up to 6 are free

    timetable = _solve(problem, constraints=_with(NO_WEIGHTS, NoLateFriday("T1")))
    assert timetable.status.has_solution
    assert validate_timetable(problem, timetable) == []  # the 10 default hard constraints still hold
    assert all(lesson.slot.day == Weekday.FRI for lesson in timetable.lessons)
    assert all(lesson.slot.period < 5 for lesson in timetable.lessons)


def test_custom_hard_makes_overfull_friday_infeasible():
    """5 Friday periods become impossible under the custom hard constraint (periods 1–4 only)."""
    problem = _friday_only_problem(periods_per_week=5)
    assert _solve(problem).status.has_solution  # possible without it

    infeasible = _solve(problem, constraints=_with(NO_WEIGHTS, NoLateFriday("T1")))
    assert infeasible.status == SolveStatus.INFEASIBLE
    assert infeasible.lessons == ()


# ---------------------------------------------------------------- custom soft injection


def test_custom_soft_steers_away_from_first_period():
    """A period-1 penalty pushes lessons to period 2 and later, and the penalty is reported as 0."""
    problem = make_problem([course(periods_per_week=2, max_periods_per_day=2)], [teacher()], days=1, periods=3)
    weights = ObjectiveWeights(**dict.fromkeys(NO_WEIGHTS.model_dump(), 0), first_period_load=1)

    timetable = _solve(problem, weights=weights, constraints=_with(weights, FirstPeriodLoad()))
    assert timetable.status.has_solution
    assert {lesson.slot.period for lesson in timetable.lessons} == {2, 3}
    assert timetable.penalties["first_period_load"] == 0
    # Default terms are reported as well (terms with nothing to count are omitted as before).
    assert {"teacher_gap", "teacher_workday", "class_gap"} <= set(timetable.penalties)


def test_custom_soft_counts_forced_first_period():
    """A lesson fixed to period 1 counts as custom penalty 1."""
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
    """A custom term without a weight enters the objective with weight 1."""
    problem = make_problem([course(periods_per_week=2, max_periods_per_day=2)], [teacher()], days=1, periods=3)

    timetable = _solve(problem, constraints=_with(NO_WEIGHTS, FirstPeriodLoad()))
    assert timetable.status.has_solution
    assert {lesson.slot.period for lesson in timetable.lessons} == {2, 3}
    assert timetable.penalties["first_period_load"] == 0


# ---------------------------------------------------------------- weight mapping


def test_weight_for_declared_extra_and_unknown_names():
    weights = ObjectiveWeights(first_period_load=2)
    assert weights.weight_for("teacher_gap") == 20  # declared fields keep their defaults
    assert weights.weight_for("first_period_load") == 2  # a custom term given as an extra
    assert weights.weight_for("unknown_term") == 1  # an unspecified custom term is 1


def test_invalid_extra_weight_rejected():
    with pytest.raises(ValueError, match="non-negative integer"):
        ObjectiveWeights(first_period_load=-1)
    with pytest.raises(ValueError, match="non-negative integer"):
        ObjectiveWeights(first_period_load="heavy")


def test_instance_weight_wins_over_objective_weights():
    """When a constraint instance carries a weight, that is the source."""
    problem = make_problem(
        [course(periods_per_week=1, fixed_slots=frozenset({slot(0, 1)}))], [teacher()], days=1, periods=3
    )
    weights = ObjectiveWeights(**dict.fromkeys(NO_WEIGHTS.model_dump(), 0), first_period_load=1)

    # ObjectiveWeights says 1, but the instance carries 5.
    timetable = _solve(problem, weights=weights, constraints=_with(weights, WeightedFirstPeriodLoad(weight=5)))
    assert timetable.penalties["first_period_load"] == 1
    assert timetable.objective_value == 5


# ---------------------------------------------------------------- relaxing H1 (hard <-> soft)


def test_weekly_period_counts_can_be_relaxed_into_a_penalty():
    """The same constraint with hard=False allows unassigned lessons and counts them as a penalty."""
    # 3 periods but only 2 periods a day -> impossible as hard, 1 period unassigned when relaxed
    problem = make_problem([course(periods_per_week=3, max_periods_per_day=3)], [teacher()], days=1, periods=2)

    assert _solve(problem).status == SolveStatus.INFEASIBLE

    relaxed = merge(default_constraints(NO_WEIGHTS), WeeklyPeriodCounts(hard=False, weight=100))
    timetable = _solve(problem, constraints=relaxed)
    assert timetable.status.has_solution
    assert len(timetable.lessons) == 2
    assert timetable.penalties["weekly_period_counts"] == 1
    assert timetable.objective_value == 100


def test_merge_replaces_in_place_and_appends_new_names():
    """merge replaces same-named entries in place and appends only new names."""
    base = default_constraints()
    merged = merge(base, WeeklyPeriodCounts(hard=False), NoLateFriday("T1"))

    assert [c.name for c in merged] == [*[c.name for c in base], "no_late_friday"]
    assert merged[0] == WeeklyPeriodCounts(hard=False)
