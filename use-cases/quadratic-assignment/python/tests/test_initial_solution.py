import pytest

from quadratic_assignment import SolverConfig, calculate_score, solve, with_initial_solution
from quadratic_assignment.models import QuadraticAssignmentProblemModel


def _problem(**fields) -> QuadraticAssignmentProblemModel:
    return QuadraticAssignmentProblemModel(
        facilities=["F1", "F2", "F3"],
        locations=["L1", "L2", "L3"],
        flows=[[0, 3, 1], [3, 0, 2], [1, 2, 0]],
        distances=[[0, 1, 4], [1, 0, 2], [4, 2, 0]],
        **fields,
    )


def _no_search() -> SolverConfig:
    return SolverConfig(max_iterations=0)


def test_complete_seed_comes_back_unchanged_with_zero_budget():
    plain = _problem()
    solve(plain, config=_no_search())
    assert plain.assignment == {"L1": "F1", "L2": "F2", "L3": "F3"}

    seed = {"L3": "F1", "L1": "F2", "L2": "F3"}
    seeded = with_initial_solution(_problem(), {"assignment": seed})
    solve(seeded, config=_no_search())
    assert seeded.assignment == seed
    assert seeded.total_cost == 2 * (3 * 4 + 1 * 2 + 2 * 1)


def test_seed_is_read_by_location_id_not_by_position():
    problem = _problem()
    problem.locations.reverse()
    problem.distances = [row[::-1] for row in problem.distances[::-1]]
    seed = {"L1": "F2", "L2": "F3", "L3": "F1"}
    seeded = with_initial_solution(problem, {"assignment": seed})
    solve(seeded, config=_no_search())
    assert seeded.assignment == seed


def test_complete_seed_keeps_a_broken_fixed_assignment_with_zero_budget():
    problem = _problem(fixed_assignments={"L1": "F1"})
    seeded = with_initial_solution(problem, {"assignment": {"L1": "F2", "L2": "F1", "L3": "F3"}})
    solve(seeded, config=_no_search())
    assert seeded.assignment["L1"] == "F2"
    assert seeded.score_hard == -1


@pytest.mark.parametrize("as_dict", [False, True])
def test_solved_result_feeds_back(as_dict):
    solved = _problem()
    solve(solved, time_limit_seconds=0.05, random_seed=3)
    solution = solved.model_dump(mode="json") if as_dict else solved
    seeded = with_initial_solution(_problem(), solution)
    assert seeded.warm_start is True
    solve(seeded, config=_no_search())
    assert seeded.assignment == solved.assignment
    assert seeded.total_cost == solved.total_cost


def test_search_from_a_seed_never_scores_below_it():
    seeded = with_initial_solution(_problem(), {"assignment": {"L1": "F3", "L2": "F1", "L3": "F2"}})
    seed_score = calculate_score(seeded.model_copy(deep=True))
    assert tuple(solve(seeded, config=SolverConfig(max_iterations=200, random_seed=1))) >= tuple(seed_score)


def test_partial_assignment_raises_and_names_the_location():
    with pytest.raises(ValueError, match="'L3'"):
        with_initial_solution(_problem(), {"assignment": {"L1": "F1", "L2": "F2"}})


@pytest.mark.parametrize(
    "assignment, message",
    [
        ({"L1": "F1", "L2": "F2", "L9": "F3"}, "unknown location: 'L9'"),
        ({"L1": "F1", "L2": "F2", "L3": "F9"}, "unknown facility: 'F9'"),
        ({"L1": "F1", "L2": "F2", "L3": 3}, "'L3'"),
        ({"L1": "F1", "L2": "F1", "L3": "F3"}, "'F1' is assigned to both 'L1' and 'L2'"),
    ],
)
def test_invalid_entries_raise(assignment, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(_problem(), {"assignment": assignment})


@pytest.mark.parametrize("solution", [{}, [], None, {"assignment": []}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError, match="'assignment' mapping"):
        with_initial_solution(_problem(), solution)


def test_hard_infeasible_result_feeds_back_unchanged():
    problem = _problem(fixed_assignments={"L1": "F1", "L2": "F1"})
    solved = problem.model_copy(deep=True)
    solve(solved, config=SolverConfig(max_iterations=50, random_seed=1))
    assert solved.score_hard < 0
    seeded = with_initial_solution(problem, solved.model_dump(mode="json"))
    solve(seeded, config=_no_search())
    assert seeded.assignment == solved.assignment
    assert (seeded.score_hard, seeded.score_soft) == (solved.score_hard, solved.score_soft)


def test_input_problem_is_not_mutated():
    problem = _problem()
    solve(problem, config=_no_search())
    before = problem.model_dump()
    seeded = with_initial_solution(problem, {"assignment": {"L1": "F3", "L2": "F1", "L3": "F2"}})
    solve(seeded, config=_no_search())
    assert problem.model_dump() == before


def test_plain_solve_still_ignores_input_assignment():
    fresh = _problem()
    preset = _problem(assignment={"L1": "F3", "L2": "F1", "L3": "F2"})
    assert preset.warm_start is False
    solve(fresh, config=_no_search())
    solve(preset, config=_no_search())
    assert fresh.assignment == preset.assignment == {"L1": "F1", "L2": "F2", "L3": "F3"}


def test_a_seed_is_used_once():
    problem = _problem()
    solve(problem, config=_no_search())
    seeded = with_initial_solution(_problem(), problem)
    assert seeded.warm_start is True
    solve(seeded, config=_no_search())
    assert seeded.warm_start is False
