"""A parameter that acts only through a cost needs that cost in the selection."""

import pytest

import optimaru.assembly_line_balancing as alb
from optimaru.assembly_line_balancing.constraints import get_constraints
from optimaru.assembly_line_balancing.models import AssemblyLineBalancingProblemModel, parameter_uses

from .._picks import replace_default, without


def problem(**fields) -> AssemblyLineBalancingProblemModel:
    tasks = [
        {"id": "a", "duration": 3, "min_station": 1, "max_station": 2, "forbidden_stations": [0]},
        {"id": "b", "duration": 2, "fixed_station": 1},
        {"id": "c", "duration": 4},
    ]
    data = {
        "cycle_time": 6,
        "tasks": tasks,
        "precedences": [{"before": "a", "after": "c"}],
        "max_stations": 3,
        "same_station_groups": [{"id": "g", "task_ids": ["a", "b"]}],
        "different_station_pairs": [{"first": "b", "second": "c"}],
    } | fields
    return AssemblyLineBalancingProblemModel.model_validate(data)


CHECKED = [
    ("precedences", "task_precedence_cost"),
    ("max_stations", "max_stations_cost"),
    ("same_station_groups", "same_station_groups_cost"),
    ("different_station_pairs", "different_station_pairs_cost"),
    ("min_station", "task_station_bounds_cost"),
    ("fixed_station", "fixed_task_stations_cost"),
    ("forbidden_stations", "forbidden_task_stations_cost"),
]


def test_the_default_selection_accepts_every_parameter():
    assert {use.field for use in parameter_uses(problem())} >= {field for field, _ in CHECKED} | {"max_station"}
    alb.solve(problem(), time_limit_seconds=0)


@pytest.mark.parametrize("field, cost", CHECKED)
def test_a_selection_without_the_cost_is_refused(field, cost):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        alb.solve(problem(), time_limit_seconds=0, constraints=without(get_constraints(), cost))


@pytest.mark.parametrize("field, cost", CHECKED)
def test_the_cost_at_weight_zero_counts(field, cost):
    picks = replace_default(
        get_constraints(), cost, type(next(c for c in get_constraints() if c.name == cost))(weight=0)
    )
    alb.solve(problem(), time_limit_seconds=0, constraints=picks)


def test_without_the_parameters_no_cost_is_needed():
    bare = AssemblyLineBalancingProblemModel.model_validate(
        {"cycle_time": 6, "tasks": [{"id": "a", "duration": 3}, {"id": "b", "duration": 2}]}
    )
    assert parameter_uses(bare) == []
    alb.solve(
        bare, time_limit_seconds=0, constraints=[c for c in get_constraints() if c.name == "station_capacity_cost"]
    )


def test_an_engine_object_is_not_checked():
    alb.solve(problem().to_core(), time_limit_seconds=0, constraints=without(get_constraints(), "max_stations_cost"))
