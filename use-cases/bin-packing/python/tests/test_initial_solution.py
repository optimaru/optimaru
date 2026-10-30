import pytest

from bin_packing import BinPackingProblem, Item, SolverConfig, _core, solve, with_initial_solution
from bin_packing.models import BinPackingProblemModel


def _problem() -> BinPackingProblemModel:
    return BinPackingProblemModel(
        bin_capacity=10,
        items=[
            {"id": "A", "weight": 8},
            {"id": "B", "weight": 7},
            {"id": "C", "weight": 3},
            {"id": "D", "weight": 2},
        ],
    )


def _no_search() -> SolverConfig:
    return SolverConfig(max_iterations=0)


def _bins(problem) -> dict[str, int]:
    return {item.id: item.bin for item in problem.items}


def test_complete_seed_comes_back_unchanged_with_zero_budget():
    plain = _problem().to_core()
    solve(plain, config=_no_search())
    assert _bins(plain) == {"A": 0, "B": 1, "C": 1, "D": 0}

    seeded = with_initial_solution(
        _problem(),
        {
            "items": [
                {"id": "D", "bin": 2},
                {"id": "C", "bin": 2},
                {"id": "B", "bin": 1},
                {"id": "A", "bin": 0},
            ]
        },
    ).to_core()
    solve(seeded, config=_no_search())
    assert _bins(seeded) == {"A": 0, "B": 1, "C": 2, "D": 2}
    assert seeded.bins_used() == 3


def test_complete_seed_keeps_an_overloaded_bin_with_zero_budget():
    solution = {"items": [{"id": item_id, "bin": 0} for item_id in "ABCD"]}
    seeded = with_initial_solution(_problem(), solution).to_core()
    score = solve(seeded, config=_no_search())
    assert _bins(seeded) == {"A": 0, "B": 0, "C": 0, "D": 0}
    assert score.hard == -10


@pytest.mark.parametrize("as_dict", [False, True])
def test_solved_result_feeds_back(as_dict):
    core = _problem().to_core()
    solve(core, time_limit_seconds=0.05, random_seed=3)
    solved = BinPackingProblemModel.from_core(core)
    solution = solved.model_dump(mode="json") if as_dict else solved
    seeded = with_initial_solution(_problem(), solution).to_core()
    assert solve(seeded, config=_no_search()) == core.score
    assert _bins(seeded) == _bins(core)


def test_search_from_a_seed_never_scores_below_it():
    solution = {"items": [{"id": item_id, "bin": index} for index, item_id in enumerate("ABCD")]}
    seeded = with_initial_solution(_problem(), solution).to_core()
    seed_score = _core.calculate_score(seeded)
    assert solve(seeded, config=SolverConfig(max_iterations=500, random_seed=1)) >= seed_score


def test_partial_seed_is_completed_by_first_fit_before_the_search():
    seeded = with_initial_solution(_problem(), {"items": [{"id": "B", "bin": 0}, {"id": "D", "bin": 0}]}).to_core()
    solve(seeded, config=_no_search())
    assert _bins(seeded) == {"A": 1, "B": 0, "C": 2, "D": 0}


def test_partial_seed_never_reaches_the_moves_with_minus_one(monkeypatch):
    seen = []
    empty_bin = _core._try_empty_bin
    compact = _core._compact

    def checked_empty_bin(assignments, *args):
        seen.append(min(assignments))
        return empty_bin(assignments, *args)

    def checked_compact(assignments):
        seen.append(min(assignments))
        return compact(assignments)

    monkeypatch.setattr(_core, "_try_empty_bin", checked_empty_bin)
    monkeypatch.setattr(_core, "_compact", checked_compact)
    seeded = with_initial_solution(_problem(), {"items": [{"id": "A", "bin": 0}]}).to_core()
    solve(seeded, config=SolverConfig(max_iterations=300, random_seed=2))
    assert seen
    assert min(seen) >= 0
    assert all(item.bin >= 0 for item in seeded.items)


def test_stagnation_restarts_from_the_seed_not_from_first_fit(monkeypatch):
    calls = []
    first_fit = _core._first_fit

    def recording_first_fit(weights, capacity, order, seed=None):
        calls.append(seed)
        return first_fit(weights, capacity, order, seed)

    monkeypatch.setattr(_core, "_first_fit", recording_first_fit)
    seeded = with_initial_solution(
        _problem(),
        {"items": [{"id": "A", "bin": 0}, {"id": "B", "bin": 1}, {"id": "C", "bin": 2}, {"id": "D", "bin": 2}]},
    ).to_core()
    solve(seeded, config=SolverConfig(max_iterations=200, stagnation_limit=1, random_seed=4))
    assert calls == [[0, 1, 2, 2]]


def test_warm_start_round_trips_through_the_core_problem():
    seeded = with_initial_solution(_problem(), {"items": [{"id": "A", "bin": 0}]})
    core = seeded.to_core()
    assert core.warm_start is True
    assert BinPackingProblemModel.from_core(core).warm_start is True
    assert _problem().to_core().warm_start is False
    assert BinPackingProblemModel.from_core(_problem().to_core()).warm_start is False


def test_gapped_seed_bins_are_kept_and_searched():
    solution = {"items": [{"id": "A", "bin": 3}, {"id": "B", "bin": 1}, {"id": "C", "bin": 1}, {"id": "D", "bin": 3}]}
    seeded = with_initial_solution(_problem(), solution).to_core()
    solve(seeded, config=_no_search())
    assert _bins(seeded) == {"A": 3, "B": 1, "C": 1, "D": 3}
    searched = with_initial_solution(_problem(), solution).to_core()
    assert solve(searched, config=SolverConfig(max_iterations=300, random_seed=1)).hard == 0


def test_item_at_minus_one_counts_as_left_out_and_prior_bins_are_replaced():
    problem = _problem()
    for item in problem.items:
        item.bin = 3
    seeded = with_initial_solution(problem, {"items": [{"id": "A", "bin": -1}, {"id": "B", "bin": 0}]})
    assert _bins(seeded) == {"A": -1, "B": 0, "C": -1, "D": -1}


@pytest.mark.parametrize(
    "entry, message",
    [
        ({"id": "Z", "bin": 0}, "unknown item"),
        ({"bin": 0}, "unknown item"),
        ("A", "unknown item"),
        ({"id": "A", "bin": -2}, "'A'"),
        ({"id": "A", "bin": 4}, "'A'"),
        ({"id": "A", "bin": "0"}, "'A'"),
        ({"id": "A", "bin": True}, "'A'"),
    ],
)
def test_invalid_entries_raise(entry, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(_problem(), {"items": [entry]})


def test_duplicate_item_raises():
    with pytest.raises(ValueError, match="'A' more than once"):
        with_initial_solution(_problem(), {"items": [{"id": "A", "bin": 0}, {"id": "A", "bin": 1}]})


@pytest.mark.parametrize("solution", [{}, [], None, {"items": {}}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError, match="'items' list"):
        with_initial_solution(_problem(), solution)


def test_hard_infeasible_result_feeds_back_unchanged():
    problem = BinPackingProblemModel(bin_capacity=5, items=[{"id": "A", "weight": 9}, {"id": "B", "weight": 2}])
    core = problem.to_core()
    score = solve(core, config=SolverConfig(max_iterations=50, random_seed=1))
    assert score.hard < 0
    seeded = with_initial_solution(problem, BinPackingProblemModel.from_core(core).model_dump(mode="json")).to_core()
    assert solve(seeded, config=_no_search()) == score
    assert _bins(seeded) == _bins(core)


def test_input_problem_is_not_mutated():
    core = _problem().to_core()
    solve(core, config=_no_search())
    problem = BinPackingProblemModel.from_core(core)
    before = problem.model_dump()
    seeded = with_initial_solution(problem, {"items": [{"id": "A", "bin": 1}]})
    assert seeded.score_hard is None
    assert seeded.bins_used is None
    solve(seeded.to_core(), config=_no_search())
    assert problem.model_dump() == before


def test_plain_solve_still_ignores_input_bins():
    fresh = BinPackingProblem(10, [Item("A", 8), Item("B", 7), Item("C", 3), Item("D", 2)])
    preset = BinPackingProblem(10, [Item("A", 8, 0), Item("B", 7, 0), Item("C", 3, 0), Item("D", 2, 0)])
    assert preset.warm_start is False
    assert solve(fresh, time_limit_seconds=0.05, random_seed=5) == solve(preset, time_limit_seconds=0.05, random_seed=5)
    assert _bins(fresh) == _bins(preset)


def test_a_seed_is_used_once():
    seeded = with_initial_solution(_problem(), {"items": [{"id": "A", "bin": 0}]}).to_core()
    assert seeded.warm_start is True
    solve(seeded, config=_no_search())
    assert seeded.warm_start is False
