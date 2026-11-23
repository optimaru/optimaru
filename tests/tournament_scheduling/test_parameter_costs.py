"""A parameter that acts only through a cost needs that cost in the selection."""

import pytest

import optimaru.tournament_scheduling as ts
from optimaru.tournament_scheduling.constraints import TeamUnavailabilityCost, get_constraints
from optimaru.tournament_scheduling.models import generate_demo_data, parameter_uses

QUICK = {"time_limit_seconds": 0.05, "random_seed": 1}


def without(name):
    return [spec for spec in get_constraints() if spec.name != name]


def test_the_default_selection_takes_the_penalties():
    problem = generate_demo_data()
    assert [use.field for use in parameter_uses(problem)] == ["unavailability_penalties"]
    ts.solve(problem, **QUICK)


def test_a_selection_without_the_cost_is_refused():
    with pytest.raises(
        ValueError, match="sets unavailability_penalties, but the constraints leave out team_unavailability_cost"
    ):
        ts.solve(generate_demo_data(), constraints=without("team_unavailability_cost"), **QUICK)


def test_a_weight_zero_pick_counts():
    picks = [*without("team_unavailability_cost"), TeamUnavailabilityCost(weight=0)]
    ts.solve(generate_demo_data(), constraints=picks, **QUICK)


def test_a_missing_cost_is_fine_when_its_parameter_is_unset():
    problem = generate_demo_data().model_copy(update={"unavailability_penalties": []})
    assert parameter_uses(problem) == []
    ts.solve(problem, constraints=without("team_unavailability_cost"), **QUICK)
