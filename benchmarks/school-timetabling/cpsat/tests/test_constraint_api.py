"""Constraint DTOs / catalog: does the runtime assembly agree with the constraint classes?

The DTOs are not generated code; they are built on the spot from
`registry.CONSTRAINT_TYPES`. So the check here is not "is the generated code
current" but "do the constraint classes and the DTOs say the same thing".
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


# ---------------------------------------------------------------- DTO <-> constraint


def test_every_constraint_type_has_a_dto():
    assert len(CONSTRAINT_DTOS) == len(CONSTRAINT_TYPES)


@pytest.mark.parametrize("constraint_type", CONSTRAINT_TYPES, ids=lambda t: t.name)
def test_dto_fields_match_the_constraint_fields(constraint_type):
    """Adding a parameter to a constraint makes the DTO follow on the spot — nothing else to touch."""
    dto = next(d for d in CONSTRAINT_DTOS if d.__name__ == f"{constraint_type.__name__}Dto")

    expected = {f.name for f in fields(constraint_type)}
    assert set(dto.model_fields) - {"type"} == expected


@pytest.mark.parametrize("constraint", default_constraints(), ids=lambda c: c.name)
def test_default_constraints_round_trip_through_their_dto(constraint):
    payload = {"type": constraint.name, **{f.name: getattr(constraint, f.name) for f in fields(constraint)}}
    assert to_constraints(_LIST.validate_python([payload])) == (constraint,)


def test_parameters_are_optional_in_the_payload():
    """An omitted parameter keeps the constraint class's default."""
    (restored,) = to_constraints(_LIST.validate_python([{"type": "weekly_period_counts"}]))
    assert restored == WeeklyPeriodCounts()
    assert restored.hard is True


def test_unknown_constraint_name_is_rejected():
    with pytest.raises(ValidationError):
        _LIST.validate_python([{"type": "no_such_constraint"}])


def test_unknown_parameter_is_rejected():
    """A misspelt parameter must not be silently ignored."""
    with pytest.raises(ValidationError):
        _LIST.validate_python([{"type": "teacher_gap", "wieght": 3}])


def test_hard_only_constraint_takes_no_parameters():
    with pytest.raises(ValidationError):
        _LIST.validate_python([{"type": "parallel_groups", "hard": False}])


# ---------------------------------------------------------------- catalog


def test_catalog_classifies_hard_soft_and_either():
    kinds = {spec.name: spec.kind for spec in catalog()}
    assert kinds["weekly_period_counts"] == "either", "H1 can be relaxed"
    assert kinds["parallel_groups"] == "hard"
    assert kinds["teacher_gap"] == "soft"


def test_catalog_covers_every_registered_constraint():
    assert {spec.name for spec in catalog()} == {c.name for c in CONSTRAINT_TYPES}
    assert all(spec.label for spec in catalog()), "labels shown to users must not be empty"


def test_catalog_default_weight_matches_the_class_default():
    weights = {spec.name: spec.default_weight for spec in catalog()}
    assert weights["teacher_gap"] == TeacherGap().weight
    assert weights["parallel_groups"] == 0, "hard constraints without a weight are 0"


# ---------------------------------------------------------------- end to end


def test_solve_driven_entirely_by_dtos():
    """Solves with only the constraint list received through the API — exactly what a caller does."""
    problem = make_problem([course(periods_per_week=3, max_periods_per_day=3)], [teacher()], days=1, periods=2)
    config = SolverConfig(time_limit_seconds=5.0, num_workers=4, weights=NO_WEIGHTS)

    payload = [{"type": "weekly_period_counts", "hard": False, "weight": 100}]
    overrides = to_constraints(_LIST.validate_python(payload))

    timetable = solve(problem, config, constraints=merge(default_constraints(NO_WEIGHTS), *overrides))
    assert timetable.status.has_solution
    assert timetable.penalties["weekly_period_counts"] == 1
    assert timetable.objective_value == 100


def test_dto_overrides_only_the_named_constraint():
    """Sending only some constraints keeps the remaining defaults alive."""
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
    # With H2 (fixed placement) alive, the two cells land exactly where specified.
    assert {lesson.slot.period for lesson in timetable.lessons} == {1, 3}
