"""A parameter that acts only through a cost needs that cost in a constraints= selection."""

import pytest

import optimaru.school_timetabling as st
from optimaru.school_timetabling.constraints import AvoidedPeriodCost, get_constraints
from optimaru.school_timetabling.models import TimetableProblemModel, parameter_uses

from .conftest import demo_problem

QUICK = {"time_limit_seconds": 0.1, "random_seed": 1}


def without(*names):
    return [spec for spec in get_constraints() if spec.name not in names]


def full_problem() -> dict:
    problem = demo_problem()
    teacher = problem["teachers"][1]
    teacher.update(max_periods_per_week=20, max_periods_per_day=5, max_consecutive_periods=3, preferred_free_days=[4])
    problem["class_groups"][0]["unavailable"] = [{"day": 4, "period": 4}]
    course = problem["courses"][1]
    course.update(block_periods=2, max_periods_per_day=2, forbidden_slots=[{"day": 0, "period": 1}])
    return problem


EXPECTED = {
    ("unavailable", "disallowed_slot_cost"),
    ("max_periods_per_week", "teacher_max_periods_cost"),
    ("max_periods_per_day", "teacher_max_periods_cost"),
    ("max_consecutive_periods", "teacher_max_consecutive_periods_cost"),
    ("preferred_free_days", "teacher_free_day_cost"),
    ("avoided_periods", "avoided_period_cost"),
    ("preferred_periods", "non_preferred_period_cost"),
    ("forbidden_slots", "disallowed_slot_cost"),
    ("max_periods_per_day", "course_max_periods_per_day_cost"),
    ("block_periods", "block_adjacency_cost"),
}


def test_the_uses_read_the_dict_and_the_model_alike():
    problem = full_problem()
    uses = {(use.field, use.costs[0]) for use in parameter_uses(problem)}
    assert uses == EXPECTED
    assert parameter_uses(TimetableProblemModel.model_validate(problem)) == parameter_uses(problem)


def test_the_defaults_and_a_full_selection_take_every_parameter():
    st.solve(full_problem(), **QUICK)
    st.solve(full_problem(), constraints=get_constraints(), **QUICK)


@pytest.mark.parametrize("field, cost", sorted(EXPECTED))
def test_a_selection_without_the_cost_is_refused(field, cost):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        st.solve(full_problem(), constraints=without(cost), **QUICK)


def test_fast_path_weights_of_zero_are_no_refusal():
    st.solve(full_problem(), weights={"avoided_period_cost": 0, "teacher_free_day_cost": 0}, **QUICK)


def test_a_weight_zero_pick_counts():
    st.solve(full_problem(), constraints=[*without("avoided_period_cost"), AvoidedPeriodCost(weight=0)], **QUICK)


def test_fixed_slots_and_parallel_groups_need_no_cost():
    problem = demo_problem()  # C1-PE has a fixed slot
    for subject in problem["subjects"]:
        subject.update(required_room_type=None, preferred_periods=[], avoided_periods=[])
    for teacher in problem["teachers"]:
        teacher["unavailable"] = []
    picks = without("fixed_slots_cost", "parallel_groups_cost")
    assert parameter_uses(problem) == []
    st.solve(problem, constraints=picks, **QUICK)


def test_a_zero_teacher_cap_is_the_default_as_convert_reads_it():
    problem = demo_problem()
    problem["teachers"][0].update(max_periods_per_day=0, max_consecutive_periods=0)
    assert not [
        use
        for use in parameter_uses(problem)
        if use.where == f"teacher {problem['teachers'][0]['id']}"
        and use.field in ("max_periods_per_day", "max_consecutive_periods")
    ]
