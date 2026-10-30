import pytest

from max_cut import SolverConfig, calculate_score, solve, with_initial_solution
from max_cut.models import EdgeModel, MaxCutProblemModel


def _problem(**fields) -> MaxCutProblemModel:
    return MaxCutProblemModel(
        vertices=["A", "B", "C", "D"],
        edges=[
            EdgeModel(source="A", target="B", weight=2),
            EdgeModel(source="B", target="C", weight=3),
            EdgeModel(source="C", target="D", weight=4),
        ],
        **fields,
    )


def _no_search() -> SolverConfig:
    return SolverConfig(max_iterations=0)


def test_complete_seed_comes_back_unchanged_with_zero_budget():
    plain = _problem()
    solve(plain, config=_no_search())
    assert plain.partition == {"A": 0, "B": 1, "C": 0, "D": 1}

    seed = {"D": 0, "C": 0, "B": 1, "A": 1}
    seeded = with_initial_solution(_problem(), {"partition": seed})
    solve(seeded, config=_no_search())
    assert seeded.partition == seed
    assert seeded.cut_weight == 3


@pytest.mark.parametrize("as_dict", [False, True])
def test_solved_result_feeds_back(as_dict):
    solved = _problem()
    solve(solved, time_limit_seconds=0.05, random_seed=3)
    solution = solved.model_dump(mode="json") if as_dict else solved
    seeded = with_initial_solution(_problem(), solution)
    assert seeded.warm_start is True
    solve(seeded, config=_no_search())
    assert seeded.partition == solved.partition
    assert (seeded.score_hard, seeded.score_soft) == (solved.score_hard, solved.score_soft)


def test_search_from_a_seed_never_scores_below_it():
    seeded = with_initial_solution(_problem(), {"partition": {"A": 0, "B": 0, "C": 0, "D": 0}})
    seed_score = calculate_score(seeded.model_copy(deep=True))
    assert tuple(solve(seeded, config=SolverConfig(max_iterations=500, random_seed=1))) >= tuple(seed_score)


def test_partial_seed_keeps_its_part_and_alternates_the_rest():
    seeded = with_initial_solution(_problem(), {"partition": {"A": 1, "B": 0}})
    assert seeded.partition == {"A": 1, "B": 0}
    solve(seeded, config=_no_search())
    assert seeded.partition == {"A": 1, "B": 0, "C": 0, "D": 1}


def test_vertex_left_out_takes_its_fixed_side():
    seeded = with_initial_solution(_problem(fixed_partition={"C": 1}), {"partition": {"A": 1}})
    solve(seeded, config=_no_search())
    assert seeded.partition == {"A": 1, "B": 1, "C": 1, "D": 1}


def test_seed_against_a_fixed_vertex_raises_because_the_search_cannot_repair_it():
    with pytest.raises(ValueError, match="'A' is fixed to side 1.*never moves"):
        with_initial_solution(_problem(fixed_partition={"A": 1}), {"partition": {"A": 0}})


def test_wrong_size_complete_seed_keeps_its_size_and_hard_penalty():
    problem = _problem(min_partition_size=2, max_partition_size=2)
    seed = {"A": 1, "B": 1, "C": 1, "D": 0}
    seeded = with_initial_solution(problem, {"partition": seed})
    solve(seeded, config=_no_search())
    assert seeded.partition == seed
    assert seeded.score_hard == -2
    searched = with_initial_solution(problem, {"partition": seed})
    solve(searched, config=SolverConfig(max_iterations=200, random_seed=1))
    assert sum(searched.partition.values()) == 3


def test_right_size_seed_is_kept_and_searched_under_exact_size_bounds():
    problem = _problem(min_partition_size=2, max_partition_size=2)
    seed = {"A": 1, "B": 1, "C": 0, "D": 0}
    seeded = with_initial_solution(problem, {"partition": seed})
    solve(seeded, config=_no_search())
    assert seeded.partition == seed
    searched = with_initial_solution(problem, {"partition": seed})
    solve(searched, config=SolverConfig(max_iterations=200, random_seed=1))
    assert sum(searched.partition.values()) == 2


def test_partial_seed_that_completes_to_the_wrong_size_is_accepted():
    problem = _problem(min_partition_size=2, max_partition_size=2)
    seeded = with_initial_solution(problem, {"partition": {"A": 1}})
    solve(seeded, config=_no_search())
    assert seeded.partition == {"A": 1, "B": 1, "C": 0, "D": 1}
    assert seeded.score_hard == -2


def test_wrong_size_seed_is_allowed_when_the_bounds_are_a_range():
    problem = _problem(min_partition_size=1, max_partition_size=3)
    seeded = with_initial_solution(problem, {"partition": {"A": 0, "B": 0, "C": 0, "D": 0}})
    solve(seeded, config=_no_search())
    assert seeded.score_hard == -2


def test_hard_infeasible_result_feeds_back_unchanged():
    problem = _problem(fixed_partition={"A": 1, "B": 1, "C": 1}, min_partition_size=2, max_partition_size=2)
    solved = problem.model_copy(deep=True)
    solve(solved, config=SolverConfig(max_iterations=50, random_seed=1))
    assert solved.score_hard < 0
    seeded = with_initial_solution(problem, solved.model_dump(mode="json"))
    solve(seeded, config=_no_search())
    assert seeded.partition == solved.partition
    assert (seeded.score_hard, seeded.score_soft) == (solved.score_hard, solved.score_soft)


def test_plain_solve_never_moves_a_fixed_vertex():
    for random_seed in range(5):
        problem = _problem(fixed_partition={"A": 1, "C": 1}, min_partition_size=2, max_partition_size=2)
        config = SolverConfig(max_iterations=200, random_seed=random_seed, parallel_solver_count=3)
        solve(problem, config=config)
        assert (problem.partition["A"], problem.partition["C"]) == (1, 1)


@pytest.mark.parametrize(
    "partition, message",
    [
        ({"Z": 0}, "unknown vertex"),
        ({"A": 2}, "'A'"),
        ({"A": -1}, "'A'"),
        ({"A": "1"}, "'A'"),
        ({"A": True}, "'A'"),
    ],
)
def test_invalid_entries_raise(partition, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(_problem(), {"partition": partition})


@pytest.mark.parametrize("solution", [{}, [], None, {"partition": []}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError, match="'partition' mapping"):
        with_initial_solution(_problem(), solution)


def test_input_problem_is_not_mutated():
    problem = _problem()
    solve(problem, config=_no_search())
    before = problem.model_dump()
    seeded = with_initial_solution(problem, {"partition": {"A": 1}})
    solve(seeded, config=_no_search())
    assert problem.model_dump() == before


def test_prior_partition_is_replaced():
    problem = _problem()
    solve(problem, config=_no_search())
    seeded = with_initial_solution(problem, {"partition": {"A": 1}})
    assert seeded.partition == {"A": 1}


def test_plain_solve_still_ignores_input_partition():
    fresh = _problem()
    preset = _problem(partition={"A": 1, "B": 1, "C": 1, "D": 1})
    assert preset.warm_start is False
    solve(fresh, time_limit_seconds=0.05, random_seed=5)
    solve(preset, time_limit_seconds=0.05, random_seed=5)
    assert fresh.partition == preset.partition
