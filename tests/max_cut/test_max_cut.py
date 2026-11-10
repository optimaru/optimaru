import pytest

from optimaru.max_cut import calculate_score, constraint_costs, solve
from optimaru.max_cut.models import EdgeModel, MaxCutProblemModel, VertexPairModel


def test_manual_triangle():
    problem = MaxCutProblemModel(
        vertices=["A", "B", "C"],
        edges=[
            EdgeModel(source="A", target="B", weight=2),
            EdgeModel(source="B", target="C", weight=3),
            EdgeModel(source="A", target="C", weight=4),
        ],
        partition={"A": 0, "B": 1, "C": 0},
    )
    result = calculate_score(problem)
    assert (result.hard, result.soft) == (0, 5.0)
    assert (problem.cut_weight, problem.score_hard, problem.score_soft) == (5, 0, 5.0)
    solve(problem, time_limit_seconds=0.05)
    assert problem.cut_weight == 7


def _contract() -> MaxCutProblemModel:
    return MaxCutProblemModel(
        vertices=["A", "B", "C"],
        edges=[],
        partition={"A": 0, "B": 1, "C": 1},
        fixed_partition={"A": 1},
        same_partition_pairs=[VertexPairModel(first="A", second="B")],
        different_partition_pairs=[VertexPairModel(first="B", second="C")],
        min_partition_size=1,
        max_partition_size=2,
    )


def test_partition_contract_costs_match_manual_values():
    costs = constraint_costs(_contract())
    # The unit constraints report [side 0, side 1].
    assert sum(costs["fixed_partition_cost"]["costs"]) == 1
    assert sum(costs["same_partition_pairs_cost"]["costs"]) == 1
    assert sum(costs["different_partition_pairs_cost"]["costs"]) == 1
    assert costs["fixed_partition_cost"]["costs"] == [1.0, 0.0]  # A sits on side 0
    assert costs["same_partition_pairs_cost"]["costs"] == [1.0, 0.0]  # charged to A's side
    assert costs["different_partition_pairs_cost"]["costs"] == [0.0, 1.0]  # B and C on side 1
    assert costs["partition_size_bounds_cost"]["costs"] == [0.0, 0.0]
    assert costs["partition_imbalance_cost"]["costs"] == [1.0]
    assert costs["cut_weight_cost"]["costs"] == [0.0]


def test_a_partial_partition_is_not_scored():
    """Scoring needs every vertex on a side."""
    problem = _contract()
    problem.partition = {"A": 0, "B": 1}
    with pytest.raises(ValueError, match="partition is partial"):
        calculate_score(problem)
    with pytest.raises(ValueError, match="partition is partial"):
        constraint_costs(problem)
    with pytest.raises(ValueError, match="partition is partial"):
        constraint_costs(problem.to_core())
    assert problem.cut_weight is None
    solve(problem, time_limit_seconds=0)  # solve fills the missing side in
    assert set(problem.partition) == {"A", "B", "C"}
