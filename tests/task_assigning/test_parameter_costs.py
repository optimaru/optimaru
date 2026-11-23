"""A parameter that acts only through a cost needs that cost in the selection."""

import pytest

import optimaru.task_assigning as ta
from optimaru.task_assigning.constraints import RequiredSkillCost, get_constraints
from optimaru.task_assigning.demo import generate_demo_data
from optimaru.task_assigning.models import parameter_uses

QUICK = {"time_limit_seconds": 0.05, "random_seed": 1}


def without(name):
    return [spec for spec in get_constraints() if spec.name != name]


def test_the_default_selection_takes_every_parameter():
    problem = generate_demo_data()
    fields = {(use.field, use.costs) for use in parameter_uses(problem)}
    assert fields == {
        ("required_skills", ("required_skill_cost",)),
        ("priority", ("major_priority_task_end_time_cost",)),
        ("priority", ("critical_priority_task_end_time_cost",)),
    }
    ta.solve(problem, **QUICK)


@pytest.mark.parametrize(
    "field, cost",
    [
        ("required_skills", "required_skill_cost"),
        ("priority", "major_priority_task_end_time_cost"),
        ("priority", "critical_priority_task_end_time_cost"),
    ],
)
def test_a_selection_without_the_cost_is_refused(field, cost):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        ta.solve(generate_demo_data(), constraints=without(cost), **QUICK)


def test_a_minor_task_needs_no_minor_cost():
    # MINOR is the default priority: it is not checked.
    ta.solve(generate_demo_data(), constraints=without("minor_priority_task_end_time_cost"), **QUICK)


def test_a_weight_zero_pick_counts():
    picks = [*without("required_skill_cost"), RequiredSkillCost(weight=0)]
    ta.solve(generate_demo_data(), constraints=picks, **QUICK)


def test_a_missing_cost_is_fine_when_its_parameter_is_unset():
    problem = generate_demo_data()
    problem = problem.model_copy(
        update={"task_types": [t.model_copy(update={"required_skills": []}) for t in problem.task_types]}
    )
    ta.solve(problem, constraints=without("required_skill_cost"), **QUICK)
