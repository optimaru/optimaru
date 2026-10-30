"""with_initial_solution: a seed reaches the search untouched and bad input never reaches C++."""

import pytest

pytest.importorskip("pydantic")

import meeting_scheduling as ms
from meeting_scheduling.models import MeetingScheduleModel, demo_data


def _slots(model):
    return {assignment.id: assignment.grain_room for assignment in model.meeting_assignments}


def _score(model):
    return (model.score_hard, model.score_soft)


def _value_count(model):
    return len(model.time_grains) * len(model.rooms)


def _construct(model):
    """Construction only: step_count_limit = 0 means "no limit", a zero time budget stops before the first step."""
    return ms.solve(model, time_limit_seconds=0, random_seed=1)


def _non_greedy_seed(problem):
    """Every assignment one composite value along from where construction puts it."""
    seed = _construct(problem)
    for assignment in seed.meeting_assignments:
        assignment.grain_room = (assignment.grain_room + 1) % _value_count(problem)
    return seed


def test_exported_from_the_package():
    assert "with_initial_solution" in ms.__all__


def test_complete_seed_skips_construction():
    problem = demo_data()
    seed = _non_greedy_seed(problem)
    result = _construct(ms.with_initial_solution(problem, seed))
    assert _slots(result) == _slots(seed)
    assert _slots(result) != _slots(_construct(problem))


@pytest.mark.parametrize("as_json", [False, True])
def test_solved_result_feeds_back(as_json):
    problem = demo_data()
    solved = ms.solve(problem, time_limit_seconds=0.3, random_seed=1)
    solution = solved.model_dump(mode="json") if as_json else solved
    result = _construct(ms.with_initial_solution(problem, solution))
    assert isinstance(result, MeetingScheduleModel)
    assert _slots(result) == _slots(solved)
    assert _score(result) == _score(solved)


def test_partial_seed_keeps_its_part_and_fills_the_rest():
    problem = demo_data()
    seed = _non_greedy_seed(problem)
    solution = seed.model_dump(mode="json")
    solution["meeting_assignments"] = solution["meeting_assignments"][:10]
    kept = {assignment["id"]: assignment["grain_room"] for assignment in solution["meeting_assignments"]}

    seeded = ms.with_initial_solution(problem, solution)
    assert all(value is None for assignment_id, value in _slots(seeded).items() if assignment_id not in kept)

    result = _construct(seeded)
    assert all(_slots(result)[assignment_id] == value for assignment_id, value in kept.items())
    assert all(value is not None for value in _slots(result).values())


def test_assignments_are_matched_by_id_and_prior_values_are_replaced():
    solved = _non_greedy_seed(demo_data())
    last = solved.meeting_assignments[-1]
    seeded = ms.with_initial_solution(solved, {"meeting_assignments": [{"id": last.id, "grain_room": last.grain_room}]})
    assert _slots(seeded) == {a.id: last.grain_room if a.id == last.id else None for a in solved.meeting_assignments}
    assert _score(seeded) == (None, None)


def test_unknown_assignment_raises_naming_it():
    with pytest.raises(ValueError, match="no-such-assignment"):
        ms.with_initial_solution(demo_data(), {"meeting_assignments": [{"id": "no-such-assignment"}]})


def test_composite_is_over_the_position_in_time_grains():
    """grain_index starts at 1 in the demo data; the composite counts list positions from 0."""
    problem = demo_data()
    assert problem.time_grains[0].grain_index == 1
    assignment_id = problem.meeting_assignments[0].id
    seeded = ms.with_initial_solution(
        problem, {"meeting_assignments": [{"id": assignment_id, "grain_room": _value_count(problem) - 1}]}
    )
    assert seeded.meeting_assignments[0].grain_room == _value_count(problem) - 1
    assert _construct(seeded).meeting_assignments[0].grain_room == _value_count(problem) - 1


@pytest.mark.parametrize("offset", [0, 1, 1000])
def test_value_past_the_end_raises_naming_the_assignment(offset):
    problem = demo_data()
    assignment_id = problem.meeting_assignments[0].id
    assignment = {"id": assignment_id, "grain_room": _value_count(problem) + offset}
    with pytest.raises(ValueError, match=f"'{assignment_id}'"):
        ms.with_initial_solution(problem, {"meeting_assignments": [assignment]})


@pytest.mark.parametrize("value", [-1, -2, "1", 1.0, True])
def test_value_of_the_wrong_kind_raises_naming_the_assignment(value):
    problem = demo_data()
    assignment_id = problem.meeting_assignments[0].id
    with pytest.raises(ValueError, match=f"'{assignment_id}'"):
        ms.with_initial_solution(problem, {"meeting_assignments": [{"id": assignment_id, "grain_room": value}]})


def test_repeated_assignment_raises():
    problem = demo_data()
    assignment_id = problem.meeting_assignments[0].id
    with pytest.raises(ValueError, match=f"'{assignment_id}'"):
        ms.with_initial_solution(problem, {"meeting_assignments": [{"id": assignment_id}, {"id": assignment_id}]})


def test_repeated_id_on_the_problem_raises():
    problem = demo_data()
    problem.meeting_assignments[1].id = problem.meeting_assignments[0].id
    with pytest.raises(ValueError, match=f"'{problem.meeting_assignments[0].id}'"):
        ms.with_initial_solution(problem, {"meeting_assignments": []})


@pytest.mark.parametrize("listing", ["rooms", "time_grains"])
def test_composite_over_a_different_order_raises(listing):
    problem = demo_data()
    solution = _construct(problem).model_dump(mode="json")
    solution[listing].reverse()
    with pytest.raises(ValueError, match=listing):
        ms.with_initial_solution(problem, solution)


def test_composite_over_a_different_room_count_raises():
    problem = demo_data()
    solution = _construct(problem).model_dump(mode="json")
    solution["rooms"].pop()
    with pytest.raises(ValueError, match="rooms"):
        ms.with_initial_solution(problem, solution)


@pytest.mark.parametrize(
    "solution",
    [None, [], {}, {"meeting_assignments": "x"}, {"meeting_assignments": ["x"]}, {"meeting_assignments": [{}]}],
)
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError):
        ms.with_initial_solution(demo_data(), solution)


def test_inputs_are_not_mutated():
    problem = _construct(demo_data())
    seed = _non_greedy_seed(problem)
    problem_before, seed_before = problem.model_dump(), seed.model_dump()
    seeded = ms.with_initial_solution(problem, seed)
    assert problem.model_dump() == problem_before
    assert seed.model_dump() == seed_before
    assert seeded is not problem and seeded.meeting_assignments[0] is not problem.meeting_assignments[0]


def test_pinned_assignment_keeps_its_value_whatever_the_solution_says():
    problem = demo_data()
    seed = _non_greedy_seed(problem)
    pinned_value = (seed.meeting_assignments[0].grain_room + 1) % _value_count(problem)
    problem.meeting_assignments[0].grain_room = pinned_value
    problem.meeting_assignments[0].pinned = True

    seeded = ms.with_initial_solution(problem, seed)
    assert seeded.meeting_assignments[0].grain_room == pinned_value
    assert seeded.meeting_assignments[0].pinned is True
    assert ms.solve(seeded, time_limit_seconds=0.2, random_seed=1).meeting_assignments[0].grain_room == pinned_value

    garbage = {"meeting_assignments": [{"id": problem.meeting_assignments[0].id, "grain_room": 10**9}]}
    assert ms.with_initial_solution(problem, garbage).meeting_assignments[0].grain_room == pinned_value


def test_pinned_flag_comes_from_the_problem():
    problem = demo_data()
    solution = _construct(problem).model_dump(mode="json")
    solution["meeting_assignments"][0]["pinned"] = True
    assert ms.with_initial_solution(problem, solution).meeting_assignments[0].pinned is False


def test_out_of_range_pinned_value_on_the_problem_raises():
    problem = demo_data()
    problem.meeting_assignments[0].grain_room = _value_count(problem)
    problem.meeting_assignments[0].pinned = True
    with pytest.raises(ValueError, match=f"'{problem.meeting_assignments[0].id}'"):
        ms.with_initial_solution(problem, {"meeting_assignments": []})


def test_seeded_solve_is_not_worse_than_the_seed():
    problem = demo_data()
    seeded = ms.with_initial_solution(problem, _non_greedy_seed(problem))
    start = _construct(seeded)
    assert _score(ms.solve(seeded, time_limit_seconds=0.3, random_seed=1)) >= _score(start)
