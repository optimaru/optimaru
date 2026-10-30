"""with_initial_solution: a seed reaches the search untouched and bad input never reaches C++."""

import pytest

pytest.importorskip("pydantic")

import sports_league as sl
from sports_league.models import LeagueScheduleModel


def _problem():
    return LeagueScheduleModel.from_core(sl.demo_schedule())


def _rounds(model):
    return {match.id: match.round for match in model.matches}


def _score(model):
    return (model.score_hard, model.score_soft)


def _solve(model, seconds):
    core = model.to_core()
    sl.solve(core, time_limit_seconds=seconds, random_seed=1)
    return LeagueScheduleModel.from_core(core)


def _construct(model):
    """Construction only: step_count_limit = 0 means "no limit", a zero time budget stops before the first step."""
    return _solve(model, 0)


def _non_greedy_seed(problem):
    """Every match one round along from where construction puts it."""
    seed = _construct(problem)
    for match in seed.matches:
        match.round = (match.round + 1) % len(problem.rounds)
    return seed


def test_exported_from_the_package():
    assert "with_initial_solution" in sl.__all__


def test_complete_seed_skips_construction():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    result = _construct(sl.with_initial_solution(problem, seed))
    assert _rounds(result) == _rounds(seed)
    assert _rounds(result) != _rounds(_construct(problem))


@pytest.mark.parametrize("as_json", [False, True])
def test_solved_result_feeds_back(as_json):
    problem = _problem()
    solved = _solve(problem, 0.3)
    solution = solved.model_dump(mode="json") if as_json else solved
    result = _construct(sl.with_initial_solution(problem, solution))
    assert _rounds(result) == _rounds(solved)
    assert _score(result) == _score(solved)


def test_partial_seed_keeps_its_part_and_fills_the_rest():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    solution = seed.model_dump(mode="json")
    solution["matches"] = solution["matches"][:90]
    kept = {match["id"]: match["round"] for match in solution["matches"]}

    seeded = sl.with_initial_solution(problem, solution)
    assert all(value == -1 for match_id, value in _rounds(seeded).items() if match_id not in kept)

    result = _construct(seeded)
    assert all(_rounds(result)[match_id] == value for match_id, value in kept.items())
    assert all(value >= 0 for value in _rounds(result).values())


def test_matches_are_matched_by_id_and_prior_values_are_replaced():
    solved = _non_greedy_seed(_problem())
    last = solved.matches[-1]
    seeded = sl.with_initial_solution(solved, {"matches": [{"id": last.id, "round": last.round}]})
    assert _rounds(seeded) == {m.id: last.round if m.id == last.id else -1 for m in solved.matches}
    assert _score(seeded) == (None, None)


def test_unknown_match_raises_naming_it():
    with pytest.raises(ValueError, match="no-such-match"):
        sl.with_initial_solution(_problem(), {"matches": [{"id": "no-such-match", "round": 0}]})


def test_round_is_a_position_not_a_round_index():
    """Round.index runs past the list here, so an index-based check would accept the wrong values."""
    problem = _problem()
    for round_ in problem.rounds:
        round_.index += 100
    match_id = problem.matches[0].id
    last_position = len(problem.rounds) - 1
    seeded = sl.with_initial_solution(problem, {"matches": [{"id": match_id, "round": last_position}]})
    assert seeded.matches[0].round == last_position
    with pytest.raises(ValueError, match=match_id):
        sl.with_initial_solution(problem, {"matches": [{"id": match_id, "round": problem.rounds[0].index}]})


@pytest.mark.parametrize("offset", [0, 1, 1000])
def test_value_past_the_end_raises_naming_the_match(offset):
    problem = _problem()
    match_id = problem.matches[0].id
    with pytest.raises(ValueError, match=match_id):
        sl.with_initial_solution(problem, {"matches": [{"id": match_id, "round": len(problem.rounds) + offset}]})


@pytest.mark.parametrize("value", [-2, None, "1", 1.0, True])
def test_value_of_the_wrong_kind_raises_naming_the_match(value):
    problem = _problem()
    match_id = problem.matches[0].id
    with pytest.raises(ValueError, match=match_id):
        sl.with_initial_solution(problem, {"matches": [{"id": match_id, "round": value}]})


def test_repeated_match_raises():
    problem = _problem()
    match_id = problem.matches[0].id
    with pytest.raises(ValueError, match=match_id):
        sl.with_initial_solution(problem, {"matches": [{"id": match_id}, {"id": match_id}]})


def test_repeated_id_on_the_problem_raises():
    problem = _problem()
    problem.matches[1].id = problem.matches[0].id
    with pytest.raises(ValueError, match=problem.matches[0].id):
        sl.with_initial_solution(problem, {"matches": []})


def test_position_over_a_different_round_order_raises():
    problem = _problem()
    solution = _construct(problem).model_dump(mode="json")
    solution["rounds"].reverse()
    with pytest.raises(ValueError, match="rounds"):
        sl.with_initial_solution(problem, solution)


@pytest.mark.parametrize("solution", [None, [], {}, {"matches": "x"}, {"matches": ["x"]}, {"matches": [{}]}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError):
        sl.with_initial_solution(_problem(), solution)


def test_inputs_are_not_mutated():
    problem = _construct(_problem())
    seed = _non_greedy_seed(problem)
    problem_before, seed_before = problem.model_dump(), seed.model_dump()
    seeded = sl.with_initial_solution(problem, seed)
    assert problem.model_dump() == problem_before
    assert seed.model_dump() == seed_before
    assert seeded is not problem and seeded.matches[0] is not problem.matches[0]


def test_seeded_solve_is_not_worse_than_the_seed():
    problem = _problem()
    seeded = sl.with_initial_solution(problem, _non_greedy_seed(problem))
    start = _construct(seeded)
    assert _score(_solve(seeded, 0.3)) >= _score(start)
