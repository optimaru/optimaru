from max_cut import calculate_score, constraint_costs, solve
from max_cut.models import EdgeModel, MaxCutProblemModel, VertexPairModel


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
    assert tuple(calculate_score(problem)) == (0, 5.0)
    solve(problem, time_limit_seconds=0.05)
    assert problem.cut_weight == 7


def test_partition_contract_costs_match_manual_values():
    problem = MaxCutProblemModel(
        vertices=["A", "B", "C"],
        edges=[],
        partition={"A": 0, "B": 1, "C": 1},
        fixed_partition={"A": 1},
        same_partition_pairs=[VertexPairModel(first="A", second="B")],
        different_partition_pairs=[VertexPairModel(first="B", second="C")],
        min_partition_size=1,
        max_partition_size=2,
    )
    costs = constraint_costs(problem)
    assert costs["fixed_partition"] == [1]
    assert costs["same_partition_pairs"] == [1]
    assert costs["different_partition_pairs"] == [1]
    assert costs["partition_size_bounds"] == [0, 0]
    assert costs["partition_balance"] == [1]
