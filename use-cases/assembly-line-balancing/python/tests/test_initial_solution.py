import pytest

from assembly_line_balancing import SolverConfig, calculate_score, solve, with_initial_solution
from assembly_line_balancing.models import AssemblyLineBalancingProblemModel, PrecedenceModel, TaskModel


def _problem(**fields) -> AssemblyLineBalancingProblemModel:
    return AssemblyLineBalancingProblemModel(
        cycle_time=5,
        tasks=[
            TaskModel(id="A", duration=3),
            TaskModel(id="B", duration=3),
            TaskModel(id="C", duration=2),
        ],
        precedences=[PrecedenceModel(before="A", after="B")],
        **fields,
    )


def _no_search() -> SolverConfig:
    return SolverConfig(max_iterations=0)


def _stations(problem) -> dict[str, int]:
    return {task.id: task.station for task in problem.tasks}


def test_complete_seed_comes_back_unchanged_with_zero_budget():
    plain = _problem()
    solve(plain, config=_no_search())
    assert _stations(plain) == {"A": 0, "B": 1, "C": 0}

    seeded = with_initial_solution(
        _problem(),
        {"tasks": [{"id": "C", "station": 1}, {"id": "B", "station": 1}, {"id": "A", "station": 0}]},
    )
    solve(seeded, config=_no_search())
    assert _stations(seeded) == {"A": 0, "B": 1, "C": 1}
    assert seeded.station_loads == [3, 5]


def test_complete_seed_keeps_a_broken_fixed_station_as_a_hard_penalty():
    problem = AssemblyLineBalancingProblemModel(
        cycle_time=5, tasks=[TaskModel(id="A", duration=3, fixed_station=1), TaskModel(id="B", duration=2)]
    )
    seeded = with_initial_solution(problem, {"tasks": [{"id": "A", "station": 0}, {"id": "B", "station": 0}]})
    solve(seeded, config=_no_search())
    assert _stations(seeded) == {"A": 0, "B": 0}
    assert seeded.score_hard == -1


@pytest.mark.parametrize("as_dict", [False, True])
def test_solved_result_feeds_back(as_dict):
    solved = _problem()
    solve(solved, time_limit_seconds=0.05, random_seed=3)
    solution = solved.model_dump(mode="json") if as_dict else solved
    seeded = with_initial_solution(_problem(), solution)
    assert seeded.warm_start is True
    solve(seeded, config=_no_search())
    assert _stations(seeded) == _stations(solved)
    assert (seeded.score_hard, seeded.score_soft) == (solved.score_hard, solved.score_soft)


def test_search_from_a_seed_never_scores_below_it():
    seeded = with_initial_solution(
        _problem(), {"tasks": [{"id": "A", "station": 0}, {"id": "B", "station": 1}, {"id": "C", "station": 2}]}
    )
    seed_score = calculate_score(seeded.model_copy(deep=True))
    assert tuple(solve(seeded, config=SolverConfig(max_iterations=500, random_seed=1))) >= tuple(seed_score)


def test_partial_seed_keeps_its_part_and_constructs_the_rest():
    seeded = with_initial_solution(_problem(), {"tasks": [{"id": "C", "station": 2}]})
    solve(seeded, config=_no_search())
    assert _stations(seeded) == {"A": 0, "B": 2, "C": 2}
    assert seeded.score_hard == 0


def test_task_at_minus_one_counts_as_left_out_and_prior_stations_are_replaced():
    problem = _problem()
    for task, station in zip(problem.tasks, [4, 4, 4], strict=True):
        task.station = station
    seeded = with_initial_solution(problem, {"tasks": [{"id": "A", "station": -1}, {"id": "B", "station": 1}]})
    assert _stations(seeded) == {"A": -1, "B": 1, "C": -1}
    solve(seeded, config=_no_search())
    assert _stations(seeded) == {"A": 0, "B": 1, "C": 0}


def test_unseeded_task_still_goes_to_its_fixed_station():
    problem = AssemblyLineBalancingProblemModel(
        cycle_time=5, tasks=[TaskModel(id="A", duration=1, fixed_station=1), TaskModel(id="B", duration=1)]
    )
    seeded = with_initial_solution(problem, {"tasks": [{"id": "B", "station": 0}]})
    solve(seeded, config=_no_search())
    assert _stations(seeded) == {"A": 1, "B": 0}


def test_station_above_the_task_count_is_accepted_without_max_stations():
    # solve can return such indices itself: its move draws from max(station) + 2.
    seeded = with_initial_solution(_problem(), {"tasks": [{"id": "A", "station": 7}]})
    assert seeded.tasks[0].station == 7


@pytest.mark.parametrize(
    "entry, message",
    [
        ({"id": "Z", "station": 0}, "unknown task"),
        ({"station": 0}, "unknown task"),
        ("A", "unknown task"),
        ({"id": "A", "station": -2}, "'A'"),
        ({"id": "A", "station": "0"}, "'A'"),
        ({"id": "A", "station": True}, "'A'"),
    ],
)
def test_invalid_entries_raise(entry, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(_problem(), {"tasks": [entry]})


def test_station_beyond_max_stations_stays_a_hard_penalty():
    seeded = with_initial_solution(_problem(max_stations=2), {"tasks": [{"id": "A", "station": 2}]})
    assert seeded.tasks[0].station == 2
    solve(seeded, config=_no_search())
    assert seeded.score_hard < 0


def test_hard_infeasible_result_feeds_back_unchanged():
    problem = AssemblyLineBalancingProblemModel(
        cycle_time=5, max_stations=1, tasks=[TaskModel(id="A", duration=3), TaskModel(id="B", duration=3)]
    )
    solved = problem.model_copy(deep=True)
    solve(solved, config=SolverConfig(max_iterations=50, random_seed=1))
    assert solved.score_hard < 0
    seeded = with_initial_solution(problem, solved.model_dump(mode="json"))
    solve(seeded, config=_no_search())
    assert _stations(seeded) == _stations(solved)
    assert (seeded.score_hard, seeded.score_soft) == (solved.score_hard, solved.score_soft)


def test_duplicate_task_raises():
    with pytest.raises(ValueError, match="'A' more than once"):
        with_initial_solution(_problem(), {"tasks": [{"id": "A", "station": 0}, {"id": "A", "station": 1}]})


@pytest.mark.parametrize("solution", [{}, [], None, {"tasks": {}}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError, match="'tasks' list"):
        with_initial_solution(_problem(), solution)


def test_input_problem_is_not_mutated():
    problem = _problem()
    solve(problem, config=_no_search())
    before = problem.model_dump()
    seeded = with_initial_solution(problem, {"tasks": [{"id": "A", "station": 1}]})
    solve(seeded, config=_no_search())
    assert problem.model_dump() == before
    assert seeded.score_hard is not None


def test_plain_solve_still_ignores_input_stations():
    fresh = _problem()
    preset = _problem()
    for task in preset.tasks:
        task.station = 2
    assert preset.warm_start is False
    solve(fresh, time_limit_seconds=0.05, random_seed=5)
    solve(preset, time_limit_seconds=0.05, random_seed=5)
    assert _stations(fresh) == _stations(preset)
