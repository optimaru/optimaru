"""with_initial_solution: a seed reaches the search untouched and bad input never reaches C++."""

import pytest

pytest.importorskip("pydantic")

import conference_scheduling as cs
from conference_scheduling.models import ConferenceScheduleModel


def _problem():
    return ConferenceScheduleModel.from_core(cs.demo_conference())


def _slots(model):
    return {talk.code: talk.slot_room for talk in model.talks}


def _score(model):
    return (model.score_hard, model.score_soft)


def _value_count(model):
    return len(model.timeslots) * len(model.rooms)


def _solve(model, seconds):
    core = model.to_core()
    cs.solve(core, time_limit_seconds=seconds, random_seed=1)
    return ConferenceScheduleModel.from_core(core)


def _construct(model):
    """Construction only: step_count_limit = 0 means "no limit", a zero time budget stops before the first step."""
    return _solve(model, 0)


def _non_greedy_seed(problem):
    """Every talk one composite value along from where construction puts it."""
    seed = _construct(problem)
    for talk in seed.talks:
        talk.slot_room = (talk.slot_room + 1) % _value_count(problem)
    return seed


def test_exported_from_the_package():
    assert "with_initial_solution" in cs.__all__


def test_complete_seed_skips_construction():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    result = _construct(cs.with_initial_solution(problem, seed))
    assert _slots(result) == _slots(seed)
    assert _slots(result) != _slots(_construct(problem))


@pytest.mark.parametrize("as_json", [False, True])
def test_solved_result_feeds_back(as_json):
    problem = _problem()
    solved = _solve(problem, 0.3)
    solution = solved.model_dump(mode="json") if as_json else solved
    result = _construct(cs.with_initial_solution(problem, solution))
    assert _slots(result) == _slots(solved)
    assert _score(result) == _score(solved)


def test_partial_seed_keeps_its_part_and_fills_the_rest():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    solution = seed.model_dump(mode="json")
    solution["talks"] = solution["talks"][:7]
    kept = {talk["code"]: talk["slot_room"] for talk in solution["talks"]}

    seeded = cs.with_initial_solution(problem, solution)
    assert all(value == -1 for code, value in _slots(seeded).items() if code not in kept)

    result = _construct(seeded)
    assert all(_slots(result)[code] == value for code, value in kept.items())
    assert all(value >= 0 for value in _slots(result).values())


def test_talks_are_matched_by_code_and_prior_values_are_replaced():
    solved = _non_greedy_seed(_problem())
    last = solved.talks[-1]
    seeded = cs.with_initial_solution(solved, {"talks": [{"code": last.code, "slot_room": last.slot_room}]})
    assert _slots(seeded) == {t.code: last.slot_room if t.code == last.code else -1 for t in solved.talks}
    assert _score(seeded) == (None, None)


def test_unknown_talk_raises_naming_it():
    with pytest.raises(ValueError, match="no-such-talk"):
        cs.with_initial_solution(_problem(), {"talks": [{"code": "no-such-talk", "slot_room": 0}]})


def test_highest_value_is_accepted():
    problem = _problem()
    code = problem.talks[0].code
    seeded = cs.with_initial_solution(problem, {"talks": [{"code": code, "slot_room": _value_count(problem) - 1}]})
    assert seeded.talks[0].slot_room == _value_count(problem) - 1


@pytest.mark.parametrize("offset", [0, 1, 1000])
def test_value_past_the_end_raises_naming_the_talk(offset):
    problem = _problem()
    code = problem.talks[0].code
    with pytest.raises(ValueError, match=code):
        cs.with_initial_solution(problem, {"talks": [{"code": code, "slot_room": _value_count(problem) + offset}]})


@pytest.mark.parametrize("value", [-2, None, "1", 1.0, True])
def test_value_of_the_wrong_kind_raises_naming_the_talk(value):
    problem = _problem()
    code = problem.talks[0].code
    with pytest.raises(ValueError, match=code):
        cs.with_initial_solution(problem, {"talks": [{"code": code, "slot_room": value}]})


def test_repeated_talk_raises():
    problem = _problem()
    code = problem.talks[0].code
    with pytest.raises(ValueError, match=code):
        cs.with_initial_solution(problem, {"talks": [{"code": code}, {"code": code}]})


def test_repeated_code_on_the_problem_raises():
    problem = _problem()
    problem.talks[1].code = problem.talks[0].code
    with pytest.raises(ValueError, match=problem.talks[0].code):
        cs.with_initial_solution(problem, {"talks": []})


@pytest.mark.parametrize("listing", ["rooms", "timeslots"])
def test_composite_over_a_different_order_raises(listing):
    problem = _problem()
    solution = _construct(problem).model_dump(mode="json")
    solution[listing].reverse()
    with pytest.raises(ValueError, match=listing):
        cs.with_initial_solution(problem, solution)


def test_composite_over_a_different_room_count_raises():
    problem = _problem()
    solution = _construct(problem).model_dump(mode="json")
    solution["rooms"].pop()
    with pytest.raises(ValueError, match="rooms"):
        cs.with_initial_solution(problem, solution)


@pytest.mark.parametrize("solution", [None, [], {}, {"talks": "x"}, {"talks": ["x"]}, {"talks": [{}]}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError):
        cs.with_initial_solution(_problem(), solution)


def test_inputs_are_not_mutated():
    problem = _construct(_problem())
    seed = _non_greedy_seed(problem)
    problem_before, seed_before = problem.model_dump(), seed.model_dump()
    seeded = cs.with_initial_solution(problem, seed)
    assert problem.model_dump() == problem_before
    assert seed.model_dump() == seed_before
    assert seeded is not problem and seeded.talks[0] is not problem.talks[0]


def test_pinned_talk_keeps_its_value_whatever_the_solution_says():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    problem.talks[0].slot_room = (seed.talks[0].slot_room + 1) % _value_count(problem)
    problem.talks[0].pinned = True
    pinned_value = problem.talks[0].slot_room

    seeded = cs.with_initial_solution(problem, seed)
    assert seeded.talks[0].slot_room == pinned_value
    assert seeded.talks[0].pinned is True
    assert _solve(seeded, 0.2).talks[0].slot_room == pinned_value

    garbage = {"talks": [{"code": problem.talks[0].code, "slot_room": 10**9, "pinned": False}]}
    assert cs.with_initial_solution(problem, garbage).talks[0].slot_room == pinned_value


def test_pinned_flag_comes_from_the_problem():
    problem = _problem()
    solution = _construct(problem).model_dump(mode="json")
    solution["talks"][0]["pinned"] = True
    assert cs.with_initial_solution(problem, solution).talks[0].pinned is False


def test_out_of_range_pinned_value_on_the_problem_raises():
    problem = _problem()
    problem.talks[0].slot_room = _value_count(problem)
    problem.talks[0].pinned = True
    with pytest.raises(ValueError, match=problem.talks[0].code):
        cs.with_initial_solution(problem, {"talks": []})


def test_seeded_solve_is_not_worse_than_the_seed():
    problem = _problem()
    seeded = cs.with_initial_solution(problem, _non_greedy_seed(problem))
    start = _construct(seeded)
    assert _score(_solve(seeded, 0.3)) >= _score(start)
