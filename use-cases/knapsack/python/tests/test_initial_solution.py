import pytest

from knapsack import SolverConfig, calculate_score, solve, with_initial_solution
from knapsack.models import ItemModel, KnapsackProblemModel


def _problem() -> KnapsackProblemModel:
    return KnapsackProblemModel(
        capacity=10,
        items=[
            ItemModel(id="A", weight=6, value=9),
            ItemModel(id="B", weight=4, value=7),
            ItemModel(id="C", weight=5, value=8),
        ],
    )


def _no_search() -> SolverConfig:
    return SolverConfig(max_iterations=0)


def _selected(problem) -> set[str]:
    return {item.id for item in problem.items if item.selected}


def test_complete_seed_comes_back_unchanged_with_zero_budget():
    plain = _problem()
    solve(plain, config=_no_search())
    assert _selected(plain) == {"B", "C"}

    seeded = with_initial_solution(
        _problem(),
        {
            "items": [
                {"id": "C", "selected": False},
                {"id": "B", "selected": False},
                {"id": "A", "selected": True},
            ]
        },
    )
    solve(seeded, config=_no_search())
    assert _selected(seeded) == {"A"}
    assert (seeded.total_weight, seeded.total_value) == (6, 9)


def test_all_false_seed_stays_empty_because_nothing_is_filled_in():
    solution = {"items": [{"id": item_id, "selected": False} for item_id in "ABC"]}
    seeded = with_initial_solution(_problem(), solution)
    solve(seeded, config=_no_search())
    assert _selected(seeded) == set()


def test_complete_seed_keeps_an_overweight_selection_with_zero_budget():
    solution = {"items": [{"id": item_id, "selected": True} for item_id in "ABC"]}
    seeded = with_initial_solution(_problem(), solution)
    solve(seeded, config=_no_search())
    assert _selected(seeded) == {"A", "B", "C"}
    assert seeded.score_hard == -5


@pytest.mark.parametrize("as_dict", [False, True])
def test_solved_result_feeds_back(as_dict):
    solved = _problem()
    solve(solved, time_limit_seconds=0.05, random_seed=3)
    solution = solved.model_dump(mode="json") if as_dict else solved
    seeded = with_initial_solution(_problem(), solution)
    assert seeded.warm_start is True
    solve(seeded, config=_no_search())
    assert _selected(seeded) == _selected(solved)
    assert (seeded.score_hard, seeded.score_soft) == (solved.score_hard, solved.score_soft)


def test_search_from_a_seed_never_scores_below_it():
    seeded = with_initial_solution(_problem(), {"items": [{"id": item_id, "selected": False} for item_id in "ABC"]})
    seed_score = calculate_score(seeded.model_copy(deep=True))
    assert tuple(solve(seeded, config=SolverConfig(max_iterations=500, random_seed=1))) >= tuple(seed_score)


def test_partial_seed_keeps_its_part_and_fills_the_rest_around_its_load():
    seeded = with_initial_solution(_problem(), {"items": [{"id": "A", "selected": True}]})
    solve(seeded, config=_no_search())
    assert _selected(seeded) == {"A", "B"}


def test_partial_seed_keeps_an_explicit_false():
    seeded = with_initial_solution(_problem(), {"items": [{"id": "B", "selected": False}]})
    solve(seeded, config=_no_search())
    assert _selected(seeded) == {"C"}


def test_entry_without_selected_counts_as_left_out():
    seeded = with_initial_solution(_problem(), {"items": [{"id": "B"}]})
    assert _selected(seeded) == {"B", "C"}


def test_prior_selection_is_replaced():
    problem = _problem()
    for item in problem.items:
        item.selected = True
    seeded = with_initial_solution(problem, {"items": [{"id": item_id, "selected": False} for item_id in "ABC"]})
    assert _selected(seeded) == set()


@pytest.mark.parametrize(
    "entry, message",
    [
        ({"id": "Z", "selected": True}, "unknown item"),
        ({"selected": True}, "unknown item"),
        ("A", "unknown item"),
        ({"id": "A", "selected": 1}, "'A'"),
        ({"id": "A", "selected": 2}, "'A'"),
        ({"id": "A", "selected": "yes"}, "'A'"),
        ({"id": "A", "selected": None}, "'A'"),
    ],
)
def test_invalid_entries_raise(entry, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(_problem(), {"items": [entry]})


def test_duplicate_item_raises():
    with pytest.raises(ValueError, match="'A' more than once"):
        with_initial_solution(_problem(), {"items": [{"id": "A", "selected": True}, {"id": "A", "selected": False}]})


@pytest.mark.parametrize("solution", [{}, [], None, {"items": {}}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError, match="'items' list"):
        with_initial_solution(_problem(), solution)


def test_hard_infeasible_result_feeds_back_unchanged():
    problem = KnapsackProblemModel(
        capacity=3,
        mandatory_items=["A"],
        items=[ItemModel(id="A", weight=5, value=1), ItemModel(id="B", weight=1, value=1)],
    )
    solved = problem.model_copy(deep=True)
    solve(solved, config=SolverConfig(max_iterations=50, random_seed=1))
    assert solved.score_hard < 0
    seeded = with_initial_solution(problem, solved.model_dump(mode="json"))
    solve(seeded, config=_no_search())
    assert _selected(seeded) == _selected(solved)
    assert (seeded.score_hard, seeded.score_soft) == (solved.score_hard, solved.score_soft)


def test_input_problem_is_not_mutated():
    problem = _problem()
    solve(problem, config=_no_search())
    before = problem.model_dump()
    seeded = with_initial_solution(problem, {"items": [{"id": "A", "selected": True}]})
    solve(seeded, config=_no_search())
    assert problem.model_dump() == before


def test_plain_solve_still_ignores_input_selection():
    fresh = _problem()
    preset = _problem()
    for item in preset.items:
        item.selected = True
    assert preset.warm_start is False
    solve(fresh, time_limit_seconds=0.05, random_seed=5)
    solve(preset, time_limit_seconds=0.05, random_seed=5)
    assert _selected(fresh) == _selected(preset)
