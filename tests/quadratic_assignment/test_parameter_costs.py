"""A parameter that acts only through a cost needs that cost in the selection."""

import pytest

import optimaru.quadratic_assignment as qap
from optimaru.quadratic_assignment.constraints import AssignmentCost, FlowDistanceCost, get_constraints
from optimaru.quadratic_assignment.models import parameter_uses

from .test_solve import demo, steps


def without(name):
    return [spec for spec in get_constraints() if spec.name != name]


def full_problem():
    problem = demo()
    size = len(problem.facilities)
    return problem.model_copy(update={"assignment_costs": [[1] * size for _ in range(size)]})


def test_the_default_selection_takes_every_parameter():
    problem = full_problem()
    assert {use.field for use in parameter_uses(problem)} == {
        "assignment_costs",
        "fixed_assignments",
        "forbidden_assignments",
        "facility_demands",
        "location_capacities",
    }
    qap.solve(problem, config=steps(50))


@pytest.mark.parametrize(
    "field, cost",
    [
        ("assignment_costs", "assignment_cost"),
        ("fixed_assignments", "pinned_assignments_cost"),
        ("forbidden_assignments", "forbidden_assignments_cost"),
        ("location_capacities", "location_capacity_cost"),
    ],
)
def test_a_selection_without_the_cost_is_refused(field, cost):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        qap.solve(full_problem(), config=steps(50), constraints=without(cost))


def test_a_weight_zero_pick_counts():
    picks = [*without("assignment_cost"), AssignmentCost(weight=0)]
    qap.solve(full_problem(), config=steps(50), constraints=picks)


def test_a_missing_cost_is_fine_when_its_parameter_is_unset():
    problem = demo().model_copy(update={"fixed_assignments": {}, "forbidden_assignments": []})
    problem = problem.model_copy(update={"facility_demands": {}, "location_capacities": {}})
    assert parameter_uses(problem) == []
    qap.solve(problem, config=steps(50), constraints=[FlowDistanceCost()])


def test_an_engine_object_is_not_checked():
    qap.solve(full_problem().to_core(), config=steps(50), constraints=[FlowDistanceCost()])
