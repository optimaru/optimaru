from itertools import product

import bin_packing as domain
import pytest
from bin_packing import constraint_costs
from bin_packing.constraints import (
    BinCapacityCost,
    ColorConflictCost,
    ItemConflictCost,
    UsedBinsCost,
    get_constraints,
    score,
)
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
        "bin_capacity_cost",
        "color_conflict_cost",
        "item_conflict_cost",
        "used_bins_cost",
    ]
    assert all(row["description"] for row in rows)


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info()."""
    engine = {row.name: row for row in domain.constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == domain.list_constraints()
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert entry["kind"] == "either"  # every bin-packing constraint is flippable
        assert entry["params"] == {"weight": None, "hard": None, "exponent": 1.0, "offset": 0.0, "clip": True}


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
        constraints=[BinCapacityCost(weight=2), UsedBinsCost(weight=3, hard=True)],
        time_limit_seconds=2,
    )
    assert solved.score_hard == -6
    assert solved.score_soft == 0


def test_exact_solver_proves_nontrivial_eight_bin_sample() -> None:
    weights = [48, 30, 19, 36, 36, 27, 42, 42, 42, 42, 42, 42, 24, 24, 51, 49, 55, 45, 33, 17]
    assert sum(weights) == 746  # ceil(746 / 100) is the manual lower bound.
    candidate = BinPackingProblemModel(
        bin_capacity=100,
        items=[ItemModel(id=str(index), weight=weight) for index, weight in enumerate(weights)],
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


def test_explicit_constraint_list_replaces_defaults() -> None:
    solved = solve(problem(), time_limit_seconds=2, constraints=[{"type": "used_bins_cost"}])
    assert solved.score_hard == 0  # capacity was not selected
    assert solved.bins_used == 1
    assert solved.score_soft == -1


def test_old_names_are_refused() -> None:
    for old in ("bin_capacity", "color_compatibility", "item_conflicts", "minimize_bins"):
        with pytest.raises(ValueError, match="unknown constraint"):
            solve(problem(), time_limit_seconds=1, constraints=[{"name": old}])


def test_exponent_and_fractional_weights_are_refused() -> None:
    with pytest.raises(ValueError, match="exponent is not supported"):
        solve(problem(), constraints=[UsedBinsCost(exponent=2.0)])
    with pytest.raises(ValueError, match="must be an integer"):
        solve(problem(), constraints=[UsedBinsCost(weight=0.5)])


def test_empty_problem() -> None:
    solved = solve(BinPackingProblemModel(bin_capacity=5, items=[]))
    assert (solved.score_hard, solved.score_soft, solved.bins_used, solved.bin_loads) == (0, 0.0, 0, [])


def _parity_problem() -> BinPackingProblemModel:
    return BinPackingProblemModel(
        bin_capacity=10,
        items=[
            ItemModel(id="A", weight=6, color="red"),
            ItemModel(id="B", weight=4, color="red"),
            ItemModel(id="C", weight=5),
            ItemModel(id="D", weight=5),
        ],
        conflicts=[ConflictModel(first="C", second="D"), ConflictModel(first="A", second="C")],
    )


def _brute_force_score(problem, constraints):
    best = None
    count = len(problem.items)
    for bins in product(range(count), repeat=count):
        candidate = problem.model_copy(deep=True)
        for item, bin_index in zip(candidate.items, bins, strict=True):
            item.bin = bin_index
        value = score(domain.constraint_costs(candidate.to_core()), constraints)
        best = max(best, value) if best is not None else value
    return best


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_brute_force_independently(constraint) -> None:
    problem = _parity_problem()
    expected = _brute_force_score(problem, [constraint])
    solved = solve(problem, time_limit_seconds=2, constraints=[constraint])
    assert (solved.score_hard, solved.score_soft) == pytest.approx(expected)


@pytest.mark.parametrize(
    "selection",
    [
        None,
        [BinCapacityCost(hard=False), ColorConflictCost(), ItemConflictCost(), UsedBinsCost()],
        [BinCapacityCost(weight=3), ColorConflictCost(hard=False, weight=2), UsedBinsCost(hard=True)],
        [ItemConflictCost(hard=False), UsedBinsCost(weight=4)],
    ],
)
def test_optimum_matches_the_engine_brute_force(selection) -> None:
    """The CP-SAT optimum is the best engine score over every assignment, under the same selection."""
    problem = _parity_problem()
    expected = _brute_force_score(problem, selection)
    solved = solve(problem, time_limit_seconds=2, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(expected))


def test_demoting_capacity_scores_like_the_engine() -> None:
    """hard=False lands on soft in both solvers: CP-SAT's reported score is the
    engine's score of the same solution under the same selection."""
    problem = _parity_problem()
    selection = [BinCapacityCost(hard=False), *get_constraints()[1:]]
    solved = solve(problem, time_limit_seconds=2, constraints=selection)
    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert solved.score_hard == 0


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------

SELECTIONS = {
    "repeat": [BinCapacityCost(), BinCapacityCost(weight=3, offset=1), UsedBinsCost()],
    "repeat-literal": [
        ColorConflictCost(offset=1, clip=False),
        ColorConflictCost(weight=2, offset=-1),
        UsedBinsCost(),
        UsedBinsCost(weight=5, offset=2, clip=False),
    ],
    "hard-offsets": [
        BinCapacityCost(offset=2, clip=False),
        ItemConflictCost(weight=2, offset=-1),
        ItemConflictCost(offset=1, clip=False),
        UsedBinsCost(hard=True, offset=1),
    ],
    "identical-twice": [ItemConflictCost(), ItemConflictCost(), UsedBinsCost()],
}


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_reach_the_brute_force_optimum(selection) -> None:
    problem = _parity_problem()
    expected = _brute_force_score(problem, selection)
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(expected))
    engine = score(domain.constraint_costs(solved.to_core(), selection), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, offset, clip} dicts pick the same instances as specs."""
    specs = [UsedBinsCost(), UsedBinsCost(weight=3, offset=1, clip=False)]
    dicts = [{"name": "used_bins_cost"}, {"name": "used_bins_cost", "weight": 3, "offset": 1, "clip": False}]
    problem = _parity_problem()
    assert solve(problem, time_limit_seconds=5, constraints=dicts) == solve(
        problem, time_limit_seconds=5, constraints=specs
    )


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(_parity_problem(), time_limit_seconds=1, constraints=[BinCapacityCost(offset=0.5)])
