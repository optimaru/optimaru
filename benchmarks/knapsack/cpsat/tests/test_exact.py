from knapsack.models import ItemModel, KnapsackProblemModel, SelectionGroupModel

from knapsack_cpsat import solve


def test_exact_matches_manual_optimum():
    result = solve(
        KnapsackProblemModel(
            capacity=10,
            items=[
                ItemModel(id="A", weight=6, value=9),
                ItemModel(id="B", weight=4, value=7),
                ItemModel(id="C", weight=5, value=8),
            ],
        )
    )
    assert (result.total_weight, result.total_value, result.score_hard, result.score_soft) == (10, 16, 0, 16.0)


def test_exact_respects_resources_groups_and_dependencies():
    result = solve(
        KnapsackProblemModel(
            capacity=8,
            resource_capacities={"labor": 3},
            mandatory_items=["A"],
            forbidden_items=["C"],
            selection_groups=[SelectionGroupModel(id="pair", item_ids=["A", "B", "C"], min_selected=2, max_selected=2)],
            items=[
                ItemModel(id="A", weight=3, value=8, resource_usage={"labor": 1}, requires=["B"]),
                ItemModel(id="B", weight=4, value=7, resource_usage={"labor": 2}),
                ItemModel(id="C", weight=1, value=100),
            ],
        )
    )
    assert {item.id for item in result.items if item.selected} == {"A", "B"}
    assert result.score_hard == 0
