"""with_initial_solution: a seed reaches the search untouched and bad input never reaches C++."""

import pytest

pytest.importorskip("pydantic")

import tournament_scheduling as ts
from tournament_scheduling.models import TournamentScheduleModel, demo_data


def _teams(model):
    return {assignment.id: assignment.team for assignment in model.team_assignments}


def _score(model):
    return (model.score_hard, model.score_soft)


def _construct(model):
    """Construction only: step_count_limit = 0 means "no limit", a zero time budget stops before the first step."""
    return ts.solve(model, time_limit_seconds=0, random_seed=1)


def _non_greedy_seed(problem):
    """Every assignment one team along from where construction puts it."""
    seed = _construct(problem)
    for assignment in seed.team_assignments:
        assignment.team = (assignment.team + 1) % len(problem.teams)
    return seed


def test_exported_from_the_package():
    assert "with_initial_solution" in ts.__all__


def test_complete_seed_skips_construction():
    problem = demo_data()
    seed = _non_greedy_seed(problem)
    result = _construct(ts.with_initial_solution(problem, seed))
    assert _teams(result) == _teams(seed)
    assert _teams(result) != _teams(_construct(problem))


@pytest.mark.parametrize("as_json", [False, True])
def test_solved_result_feeds_back(as_json):
    problem = demo_data()
    solved = ts.solve(problem, time_limit_seconds=0.3, random_seed=1)
    solution = solved.model_dump(mode="json") if as_json else solved
    result = _construct(ts.with_initial_solution(problem, solution))
    assert isinstance(result, TournamentScheduleModel)
    assert _teams(result) == _teams(solved)
    assert _score(result) == _score(solved)


def test_partial_seed_keeps_its_part_and_fills_the_rest():
    problem = demo_data()
    seed = _non_greedy_seed(problem)
    solution = seed.model_dump(mode="json")
    solution["team_assignments"] = solution["team_assignments"][:30]
    kept = {assignment["id"]: assignment["team"] for assignment in solution["team_assignments"]}

    seeded = ts.with_initial_solution(problem, solution)
    assert all(value is None for assignment_id, value in _teams(seeded).items() if assignment_id not in kept)

    result = _construct(seeded)
    assert all(_teams(result)[assignment_id] == value for assignment_id, value in kept.items())
    assert all(value is not None for value in _teams(result).values())


def test_assignments_are_matched_by_id_and_prior_values_are_replaced():
    solved = _non_greedy_seed(demo_data())
    last = solved.team_assignments[-1]
    seeded = ts.with_initial_solution(solved, {"team_assignments": [{"id": last.id, "team": last.team}]})
    assert _teams(seeded) == {a.id: last.team if a.id == last.id else None for a in solved.team_assignments}
    assert _score(seeded) == (None, None)


@pytest.mark.parametrize("assignment_id", [10_000, "0", None])
def test_unknown_assignment_raises_naming_it(assignment_id):
    with pytest.raises(ValueError, match=repr(assignment_id)):
        ts.with_initial_solution(demo_data(), {"team_assignments": [{"id": assignment_id}]})


def test_highest_value_is_accepted():
    problem = demo_data()
    last_team = len(problem.teams) - 1
    seeded = ts.with_initial_solution(problem, {"team_assignments": [{"id": 0, "team": last_team}]})
    assert seeded.team_assignments[0].team == last_team


@pytest.mark.parametrize("offset", [0, 1, 1000])
def test_value_past_the_end_raises_naming_the_assignment(offset):
    problem = demo_data()
    with pytest.raises(ValueError, match="team assignment 5"):
        ts.with_initial_solution(problem, {"team_assignments": [{"id": 5, "team": len(problem.teams) + offset}]})


def test_value_past_the_end_on_a_model_raises():
    problem = demo_data()
    seed = _construct(problem)
    seed.team_assignments[3].team = len(problem.teams)
    with pytest.raises(ValueError, match=f"team assignment {seed.team_assignments[3].id}"):
        ts.with_initial_solution(problem, seed)


@pytest.mark.parametrize("value", [-1, -2, "1", 1.0, True])
def test_value_of_the_wrong_kind_raises_naming_the_assignment(value):
    with pytest.raises(ValueError, match="team assignment 5"):
        ts.with_initial_solution(demo_data(), {"team_assignments": [{"id": 5, "team": value}]})


def test_repeated_assignment_raises():
    with pytest.raises(ValueError, match="repeats an id"):
        ts.with_initial_solution(demo_data(), {"team_assignments": [{"id": 5}, {"id": 5}]})


def test_repeated_id_on_the_problem_raises():
    problem = demo_data()
    problem.team_assignments[1].id = problem.team_assignments[0].id
    with pytest.raises(ValueError, match="repeats an id"):
        ts.with_initial_solution(problem, {"team_assignments": []})


def test_index_over_a_different_team_order_raises():
    problem = demo_data()
    solution = _construct(problem).model_dump(mode="json")
    solution["teams"].reverse()
    with pytest.raises(ValueError, match="teams"):
        ts.with_initial_solution(problem, solution)


@pytest.mark.parametrize(
    "solution", [None, [], {}, {"team_assignments": "x"}, {"team_assignments": ["x"]}, {"team_assignments": [{}]}]
)
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError):
        ts.with_initial_solution(demo_data(), solution)


def test_inputs_are_not_mutated():
    problem = _construct(demo_data())
    seed = _non_greedy_seed(problem)
    problem_before, seed_before = problem.model_dump(), seed.model_dump()
    seeded = ts.with_initial_solution(problem, seed)
    assert problem.model_dump() == problem_before
    assert seed.model_dump() == seed_before
    assert seeded is not problem and seeded.team_assignments[0] is not problem.team_assignments[0]


def test_pinned_assignment_keeps_its_team_whatever_the_solution_says():
    problem = demo_data()
    seed = _non_greedy_seed(problem)
    pinned_team = (seed.team_assignments[0].team + 1) % len(problem.teams)
    problem.team_assignments[0].team = pinned_team
    problem.team_assignments[0].pinned = True

    seeded = ts.with_initial_solution(problem, seed)
    assert seeded.team_assignments[0].team == pinned_team
    assert seeded.team_assignments[0].pinned is True
    assert ts.solve(seeded, time_limit_seconds=0.2, random_seed=1).team_assignments[0].team == pinned_team

    garbage = {"team_assignments": [{"id": problem.team_assignments[0].id, "team": 10**9}]}
    assert ts.with_initial_solution(problem, garbage).team_assignments[0].team == pinned_team


def test_pinned_flag_comes_from_the_problem():
    problem = demo_data()
    solution = _construct(problem).model_dump(mode="json")
    solution["team_assignments"][0]["pinned"] = True
    assert ts.with_initial_solution(problem, solution).team_assignments[0].pinned is False


def test_out_of_range_pinned_value_on_the_problem_raises():
    problem = demo_data()
    problem.team_assignments[0].team = len(problem.teams)
    problem.team_assignments[0].pinned = True
    with pytest.raises(ValueError, match=f"team assignment {problem.team_assignments[0].id}"):
        ts.with_initial_solution(problem, {"team_assignments": []})


def test_seeded_solve_is_not_worse_than_the_seed():
    problem = demo_data()
    seeded = ts.with_initial_solution(problem, _non_greedy_seed(problem))
    start = _construct(seeded)
    assert _score(ts.solve(seeded, time_limit_seconds=0.3, random_seed=1)) >= _score(start)
