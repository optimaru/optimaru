"""A problem parameter that only a cost reads is refused when the constraints leave that cost out."""

import pytest

import optimaru.max_cut as max_cut
from optimaru.max_cut.constraints import CutWeightCost, PartitionSizeBoundsCost, get_constraints
from optimaru.max_cut.models import (
    EdgeModel,
    MaxCutProblemModel,
    MaxCutRequest,
    VertexPairModel,
    parameter_uses,
    solve_request,
)

EVERY_PARAMETER = {
    "same_partition_pairs": [VertexPairModel(first="A", second="C")],
    "different_partition_pairs": [VertexPairModel(first="A", second="B")],
    "min_partition_size": 1,
    "max_partition_size": 3,
}
COST = {
    "same_partition_pairs": "same_partition_pairs_cost",
    "different_partition_pairs": "different_partition_pairs_cost",
    "min_partition_size": "partition_size_bounds_cost",
    "max_partition_size": "partition_size_bounds_cost",
}


def _problem(**fields) -> MaxCutProblemModel:
    return MaxCutProblemModel(
        vertices=["A", "B", "C", "D"],
        edges=[EdgeModel(source="A", target="B", weight=2), EdgeModel(source="C", target="D", weight=4)],
        **fields,
    )


def test_parameter_uses_lists_what_the_problem_sets_away_from_its_default():
    assert parameter_uses(_problem()) == []
    assert parameter_uses(_problem(fixed_partition={"A": 0})) == []  # pinned: holds whatever the selection
    assert [use.field for use in parameter_uses(_problem(**EVERY_PARAMETER))] == list(EVERY_PARAMETER)


def test_the_default_constraints_take_every_parameter():
    max_cut.solve(_problem(**EVERY_PARAMETER), time_limit_seconds=0, random_seed=1)


@pytest.mark.parametrize("field", list(EVERY_PARAMETER))
def test_a_selection_without_the_cost_is_refused(field):
    problem = _problem(**{field: EVERY_PARAMETER[field]})
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {COST[field]}"):
        max_cut.solve(problem, time_limit_seconds=0, constraints=[CutWeightCost()])
    with pytest.raises(ValueError, match=f"sets {field}"):
        solve_request(MaxCutRequest(model_input=problem, time_limit_seconds=0), constraints=[CutWeightCost()])


def test_a_weight_of_zero_counts():
    problem = _problem(min_partition_size=1)
    max_cut.solve(problem, time_limit_seconds=0, constraints=[CutWeightCost(), PartitionSizeBoundsCost(weight=0)])


def test_parameters_left_at_their_defaults_need_no_cost():
    max_cut.solve(_problem(fixed_partition={"A": 1}), time_limit_seconds=0, constraints=[CutWeightCost()])
    assert get_constraints()
