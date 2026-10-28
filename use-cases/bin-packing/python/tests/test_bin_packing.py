from bin_packing import BinPackingProblem, Item, SolverConfig, calculate_score, constraint_costs, solve
from bin_packing.constraints import BinCapacity, MinimizeBins, score
from bin_packing.models import BinPackingProblemModel, ConflictModel


def problem() -> BinPackingProblem:
    return BinPackingProblem(10, [Item("A", 8), Item("B", 7), Item("C", 3), Item("D", 2)])


def test_constraint_costs_match_hand_calculation() -> None:
    candidate = problem()
    for item, bin_index in zip(candidate.items, [0, 0, 1, 1], strict=True):
        item.bin = bin_index
    raw = constraint_costs(candidate)
    assert raw["bin_capacity"]["costs"] == [5, 0]
    assert raw["minimize_bins"]["costs"] == [2]
    assert score(raw) == (-5, -2.0)
    assert score(raw, [BinCapacity(hard=False, weight=3), MinimizeBins(hard=True, weight=2)]) == (-4, -15.0)


def test_metaheuristic_finds_two_bin_optimum() -> None:
    candidate = problem()
    solved = solve(candidate, time_limit_seconds=0.1, random_seed=7)
    assert solved.hard == 0
    assert candidate.bins_used() == 2
    assert sorted(candidate.bin_loads()) == [10, 10]


def test_metaheuristic_accepts_all_solver_parameters() -> None:
    candidate = problem()
    config = SolverConfig(
        spent_limit_seconds=0.1,
        random_seed=11,
        parallel_solver_count=3,
        max_iterations=500,
        stagnation_limit=20,
        temperature=1.5,
        cooling_rate=0.99,
        best_score_limit=(0, -2),
    )
    solve(candidate, config=config)
    assert (candidate.score.hard, candidate.score.soft) == (0, -2.0)


def test_nontrivial_sample_reaches_hand_calculated_eight_bin_lower_bound() -> None:
    weights = [48, 30, 19, 36, 36, 27, 42, 42, 42, 42, 42, 42, 24, 24, 51, 49, 55, 45, 33, 17]
    assert sum(weights) == 746  # ceil(746 / 100) proves that at least eight bins are needed.
    candidate = BinPackingProblem(100, [Item(str(index), weight) for index, weight in enumerate(weights)])
    solve(candidate, time_limit_seconds=0.2, random_seed=7, parallel_solver_count=4)
    assert candidate.score.hard == 0
    assert candidate.bins_used() == 8


def test_model_round_trip_includes_detailed_output() -> None:
    candidate = problem()
    solve(candidate, time_limit_seconds=0.05)
    model = BinPackingProblemModel.from_core(candidate)
    assert model.bins_used == 2
    assert sum(model.bin_loads or []) == 20
    assert calculate_score(model.to_core()) == candidate.score


def test_color_and_conflict_costs_match_manual_count():
    model = BinPackingProblemModel(
        bin_capacity=20,
        items=[
            {"id": "A", "weight": 3, "bin": 0, "color": "red"},
            {"id": "B", "weight": 4, "bin": 0, "color": "red"},
            {"id": "C", "weight": 5, "bin": 0, "color": "blue"},
        ],
        conflicts=[ConflictModel(first="A", second="C")],
    )
    raw = constraint_costs(model.to_core())
    assert raw["color_compatibility"]["costs"] == [1, 0]
    assert raw["item_conflicts"]["costs"] == [1]
