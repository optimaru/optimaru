from knapsack import calculate_score, constraint_costs, solve
from knapsack.models import ItemModel, KnapsackProblemModel, SelectionGroupModel


def test_manual_score_and_meta_optimum():
    problem = KnapsackProblemModel(
        capacity=10,
        items=[
            ItemModel(id="A", weight=6, value=9),
            ItemModel(id="B", weight=4, value=7),
            ItemModel(id="C", weight=5, value=8),
        ],
    )
    problem.items[0].selected = problem.items[2].selected = True
    assert tuple(calculate_score(problem)) == (-1, 17.0)
    solve(problem, time_limit_seconds=0.05, random_seed=2)
    assert (problem.total_weight, problem.total_value, problem.score_hard) in {(9, 15, 0), (10, 16, 0)}


def test_extended_constraint_costs_match_manual_values():
    problem = KnapsackProblemModel(
        capacity=10,
        resource_capacities={"labor": 2},
        mandatory_items=["C"],
        forbidden_items=["A"],
        selection_groups=[
            SelectionGroupModel(id="choose-two", item_ids=["A", "B", "C"], min_selected=2, max_selected=2)
        ],
        items=[
            ItemModel(id="A", weight=3, value=5, selected=True, resource_usage={"labor": 3}, requires=["B"]),
            ItemModel(id="B", weight=2, value=4),
            ItemModel(id="C", weight=1, value=1),
        ],
    )
    costs = constraint_costs(problem)
    assert costs["resource_capacities"] == [1]
    assert costs["mandatory_items"] == [1]
    assert costs["forbidden_items"] == [1]
    assert costs["selection_group_bounds"] == [1]
    assert costs["item_dependencies"] == [1]
