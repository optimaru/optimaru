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
    TeacherGapCost,
    UnassignedLessonsCost,
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
    """An omitted parameter keeps the constraint class's default: hard=None, the engine's default level."""
    (restored,) = to_constraints(_LIST.validate_python([{"type": "unassigned_lessons_cost"}]))
    assert restored == UnassignedLessonsCost()
    assert restored.hard is None
    assert restored.level == "hard"


def test_unknown_constraint_name_is_rejected():
    with pytest.raises(ValidationError):
        _LIST.validate_python([{"type": "no_such_constraint"}])


def test_unknown_parameter_is_rejected():
    """A misspelt parameter must not be silently ignored."""
    with pytest.raises(ValidationError):
        _LIST.validate_python([{"type": "teacher_gap_cost", "wieght": 3}])


def test_every_constraint_takes_the_three_knobs():
    """Every DTO carries the engine's knobs (weight / hard / exponent) and nothing else."""
    for dto in CONSTRAINT_DTOS:
        assert set(dto.model_fields) == {"type", "weight", "hard", "exponent"}


def test_a_structural_constraint_refuses_soft():
    """parallel_groups_cost validates as a DTO, but hard=False is refused like in the engine (not flippable)."""
    (relaxed,) = to_constraints(_LIST.validate_python([{"type": "parallel_groups_cost", "hard": False}]))
    problem = make_problem([course(periods_per_week=1)], [teacher()], days=1, periods=2)
    config = SolverConfig(time_limit_seconds=1.0, num_workers=1, weights=NO_WEIGHTS)
    with pytest.raises(ValueError, match="is always hard"):
        solve(problem, config, constraints=merge(default_constraints(NO_WEIGHTS), relaxed))


# ---------------------------------------------------------------- catalog


def test_catalog_classifies_by_the_engine_flippability():
    """kind is "either" for a flippable constraint, else its fixed level (the engine's level_flippable)."""
    kinds = {spec.name: spec.kind for spec in catalog()}
    assert kinds["unassigned_lessons_cost"] == "either", "unassigned lessons can be relaxed"
    assert kinds["teacher_gap_cost"] == "either"
    assert kinds["parallel_groups_cost"] == kinds["fixed_slots_cost"] == "hard"


def test_catalog_covers_every_registered_constraint():
    assert {spec.name for spec in catalog()} == {c.name for c in CONSTRAINT_TYPES}
    assert all(spec.label for spec in catalog()), "labels shown to users must not be empty"


def test_catalog_defaults_are_the_engine_defaults():
    """The catalog reads the engine's constraint_info(): same names, order, levels, weights, flippability."""
    import st_timetabling as st

    engine = st.constraint_info()
    assert [spec.name for spec in catalog()] == [row.name for row in engine] == list(st.list_constraints())
    for spec, row in zip(catalog(), engine, strict=True):
        assert spec.default_level == row.default_level
        assert spec.default_weight == row.default_weight
        assert spec.level_flippable == row.level_flippable
        assert spec.reward == row.reward
        assert spec.default_enabled == row.default_enabled
        assert {param.name: param.default for param in spec.params} == {"weight": None, "hard": None, "exponent": 1.0}
    assert TeacherGapCost().weight is None, "the instance keeps the engine default (weight=None)"
    assert TeacherGapCost().effective_weight == 20


# ---------------------------------------------------------------- end to end


def test_solve_driven_entirely_by_dtos():
    """Solves with only the constraint list received through the API — exactly what a caller does."""
    problem = make_problem([course(periods_per_week=3, max_periods_per_day=3)], [teacher()], days=1, periods=2)
    config = SolverConfig(time_limit_seconds=5.0, num_workers=4, weights=NO_WEIGHTS)

    payload = [{"type": "unassigned_lessons_cost", "hard": False, "weight": 100}]
    overrides = to_constraints(_LIST.validate_python(payload))

    timetable = solve(problem, config, constraints=merge(default_constraints(NO_WEIGHTS), *overrides))
    assert timetable.status.has_solution
    assert timetable.penalties["unassigned_lessons_cost"] == 1
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

    overrides = to_constraints(_LIST.validate_python([{"type": "teacher_gap_cost", "weight": 0}]))
    timetable = solve(problem, config, constraints=merge(default_constraints(NO_WEIGHTS), *overrides))

    assert timetable.status.has_solution
    # With H2 (fixed placement) alive, the two cells land exactly where specified.
    assert {lesson.slot.period for lesson in timetable.lessons} == {1, 3}
