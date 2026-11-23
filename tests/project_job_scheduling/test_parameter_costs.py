"""Project and resource parameters that only a cost reads are refused when the constraints leave it out."""

import pytest

import optimaru.project_job_scheduling as pj
from optimaru.project_job_scheduling.constraints import (
    MakespanCost,
    NonRenewableResourceCapacityCost,
    ProjectDelayCost,
    RenewableResourceCapacityCost,
)
from optimaru.project_job_scheduling.demo import generate_demo_data
from optimaru.project_job_scheduling.models import ProjectJobScheduleRequest, parameter_uses, solve_request


def _bare():
    """The demo instance with every project at release 0 and critical path 0, every resource renewable."""
    model = generate_demo_data()
    for project in model.projects:
        project.release_date = 0
        project.critical_path_duration = 0
    for resource in model.resources:
        resource.renewable = True
    return model


def test_parameter_uses_lists_project_dates_and_non_renewable_resources():
    assert parameter_uses(_bare()) == []
    fields = {use.field for use in parameter_uses(generate_demo_data())}
    assert fields == {"release_date", "critical_path_duration", "renewable"}


def test_the_default_constraints_take_every_parameter():
    pj.solve(generate_demo_data(), time_limit_seconds=0, random_seed=1)


@pytest.mark.parametrize(
    ("field", "cost", "keep"),
    [
        ("critical_path_duration", "project_delay_cost", NonRenewableResourceCapacityCost()),
        ("renewable", "non_renewable_resource_capacity_cost", ProjectDelayCost()),
    ],
)
def test_a_selection_without_the_cost_is_refused(field, cost, keep):
    model = generate_demo_data()
    picks = [RenewableResourceCapacityCost(), MakespanCost(), keep]
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        pj.solve(model, time_limit_seconds=0, constraints=picks)
    with pytest.raises(ValueError, match=f"sets {field}"):
        solve_request(ProjectJobScheduleRequest(model_input=model, time_limit_seconds=0), constraints=picks)


def test_a_weight_of_zero_counts():
    picks = [
        RenewableResourceCapacityCost(),
        NonRenewableResourceCapacityCost(weight=0),
        ProjectDelayCost(weight=0),
        MakespanCost(),
    ]
    pj.solve(generate_demo_data(), time_limit_seconds=0, constraints=picks)


def test_parameters_left_at_their_defaults_need_no_cost():
    pj.solve(_bare(), time_limit_seconds=0, constraints=[RenewableResourceCapacityCost(), MakespanCost()])
