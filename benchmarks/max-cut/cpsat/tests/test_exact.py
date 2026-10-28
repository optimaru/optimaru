from max_cut.models import EdgeModel, MaxCutProblemModel, VertexPairModel

from max_cut_cpsat import solve


def test_exact_matches_manual_triangle():
    result = solve(
        MaxCutProblemModel(
            vertices=["A", "B", "C"],
            edges=[
                EdgeModel(source="A", target="B", weight=2),
                EdgeModel(source="B", target="C", weight=3),
                EdgeModel(source="A", target="C", weight=4),
            ],
        )
    )
    assert (result.cut_weight, result.score_soft) == (7, 7.0)


def test_exact_respects_fixed_pairs_and_balance_bounds():
    result = solve(
        MaxCutProblemModel(
            vertices=["A", "B", "C", "D"],
            edges=[EdgeModel(source="A", target="C", weight=5), EdgeModel(source="B", target="D", weight=4)],
            fixed_partition={"A": 1},
            same_partition_pairs=[VertexPairModel(first="A", second="B")],
            different_partition_pairs=[VertexPairModel(first="A", second="C")],
            min_partition_size=2,
            max_partition_size=2,
        )
    )
    assert result.partition["A"] == result.partition["B"] == 1
    assert result.partition["C"] == 0
    assert sum(result.partition.values()) == 2
    assert result.score_hard == 0
