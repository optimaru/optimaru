from bin_packing import constraint_costs
from bin_packing.constraints import BinCapacity, MinimizeBins, score
from bin_packing.models import BinPackingProblemModel, ConflictModel, ItemModel

from bin_packing_cpsat import catalog, solve


def problem() -> BinPackingProblemModel:
    return BinPackingProblemModel(
        bin_capacity=10,
        items=[
            ItemModel(id="A", weight=8),
            ItemModel(id="B", weight=7),
            ItemModel(id="C", weight=3),
            ItemModel(id="D", weight=2),
        ],
    )


def test_catalog_uses_shared_constraint_names() -> None:
    rows = catalog()
    assert [row["name"] for row in rows] == [
        "bin_capacity",
        "color_compatibility",
        "item_conflicts",
        "minimize_bins",
    ]
    assert all(row["description"] for row in rows)


def test_exact_solver_matches_hand_calculated_optimum() -> None:
    solved = solve(problem(), time_limit_seconds=2, num_workers=1)
    assert solved.score_hard == 0
    assert solved.score_soft == -2
    assert solved.bins_used == 2
    assert sorted(solved.bin_loads or [])[-2:] == [10, 10]
    assert score(constraint_costs(solved.to_core())) == (0, -2.0)


def test_exact_and_domain_classes_share_override_semantics() -> None:
    solved = solve(
        problem(),
        constraints=[BinCapacity(weight=2), MinimizeBins(weight=3, hard=True)],
        time_limit_seconds=2,
    )
    assert solved.score_hard == -6
    assert solved.score_soft == 0


def test_exact_solver_proves_nontrivial_eight_bin_sample() -> None:
    weights = [
        48,
        30,
        19,
        36,
        36,
        27,
        42,
        42,
        42,
        42,
        42,
        42,
        24,
        24,
        51,
        49,
        55,
        45,
        33,
        17,
    ]
    assert sum(weights) == 746  # ceil(746 / 100) is the manual lower bound.
    candidate = BinPackingProblemModel(
        bin_capacity=100,
        items=[
            ItemModel(id=str(index), weight=weight)
            for index, weight in enumerate(weights)
        ],
    )
    solved = solve(candidate, time_limit_seconds=5, num_workers=1)
    assert solved.score_hard == 0
    assert solved.bins_used == 8


def test_colored_and_conflicting_items_use_the_same_plugins():
    solved = solve(
        BinPackingProblemModel(
            bin_capacity=20,
            items=[
                ItemModel(id="A", weight=3, color="red"),
                ItemModel(id="B", weight=4, color="red"),
                ItemModel(id="C", weight=5, color="blue"),
            ],
            conflicts=[ConflictModel(first="A", second="C")],
        )
    )
    assert solved.score_hard == 0
    assert solved.bins_used == 2
