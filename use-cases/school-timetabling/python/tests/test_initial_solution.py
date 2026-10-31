"""Starting a solve from an earlier timetable (`solve(initial_solution=...)`).

Problems here are dicts, so the keyword takes the place of the
`with_initial_solution` function of the other use cases. A zero time limit runs
construction and no search step.
"""

import copy
import json

import pytest

import st_timetabling as st
from st_timetabling.constraints import Assembly

PATHS = [pytest.param(lambda: None, id="default"), pytest.param(Assembly, id="constraints")]
MODES = ["canonical", "quickstart"]


def slots(result: dict) -> list[tuple[str, int, int]]:
    return sorted((x["course_id"], x["slot"]["day"], x["slot"]["period"]) for x in result["lessons"])


def lessons_of(entries: list[tuple[str, int, int]]) -> list[dict]:
    return [{"course_id": c, "slot": {"day": day, "period": period}} for c, day, period in entries]


def swap_days(result: dict, first: int, second: int) -> dict:
    """A complete timetable no construction would pick: two whole days trade places."""
    days = {first: second, second: first}
    seed = copy.deepcopy(result)
    for lesson in seed["lessons"]:
        lesson["slot"]["day"] = days.get(lesson["slot"]["day"], lesson["slot"]["day"])
    return seed


@pytest.fixture(scope="module")
def solved() -> dict:
    from conftest import demo_problem

    return st.solve(demo_problem(), time_limit_seconds=2.0, random_seed=1, stop_at_first_feasible=True)


@pytest.mark.parametrize("constraints", PATHS)
def test_a_complete_seed_comes_back_unchanged(problem, solved, constraints):
    constructed = st.solve(problem, time_limit_seconds=0)
    seed = swap_days(solved, 0, 1)  # day 2 holds the fixed lesson
    assert slots(seed) != slots(constructed)  # so what comes back cannot have been constructed

    result = st.solve(problem, time_limit_seconds=0, initial_solution=seed, constraints=constraints())

    assert slots(result) == slots(seed)


def test_a_complete_seed_comes_back_unchanged_in_quickstart_mode(problem, solved):
    constructed = st.solve(problem, mode="quickstart", time_limit_seconds=0)
    seed = swap_days(solved, 0, 1)
    assert slots(seed) != slots(constructed)

    assert slots(st.solve(problem, mode="quickstart", time_limit_seconds=0, initial_solution=seed)) == slots(seed)


def test_a_solved_result_feeds_back_as_a_dict_and_as_json(problem, solved):
    as_json = json.loads(json.dumps(solved))

    assert slots(st.solve(problem, time_limit_seconds=0, initial_solution=solved)) == slots(solved)
    assert slots(st.solve(problem, time_limit_seconds=0, initial_solution=as_json)) == slots(solved)


def test_room_ids_in_the_seed_are_ignored(problem, solved):
    def rooms(result: dict) -> list[tuple]:
        return sorted((x["course_id"], x["slot"]["day"], x["slot"]["period"], str(x["room_id"])) for x in result["lessons"])

    seed = copy.deepcopy(solved)
    for lesson in seed["lessons"]:
        lesson["room_id"] = "no such room"

    assert rooms(st.solve(problem, time_limit_seconds=0, initial_solution=seed)) == rooms(solved)


@pytest.mark.parametrize("mode", MODES)
def test_a_partial_seed_keeps_its_part_and_fills_the_rest(problem, solved, mode):
    dropped = [x for x in solved["lessons"] if x["course_id"] == "C2-KOR"]
    kept = [x for x in solved["lessons"] if x["course_id"] != "C2-KOR"]
    seed = {"lessons": kept}

    result = st.solve(problem, mode=mode, time_limit_seconds=0, initial_solution=seed)

    assert len(result["lessons"]) == len(solved["lessons"])
    assert set(slots({"lessons": kept})) <= set(slots(result))
    assert sum(x["course_id"] == "C2-KOR" for x in result["lessons"]) == len(dropped)


def test_a_partial_seed_fills_the_rest_on_the_constraints_path(problem, solved):
    kept = [x for x in solved["lessons"] if x["course_id"] != "C2-KOR"]

    result = st.solve(problem, time_limit_seconds=0, initial_solution={"lessons": kept}, constraints=Assembly())

    assert len(result["lessons"]) == len(solved["lessons"])
    assert set(slots({"lessons": kept})) <= set(slots(result))


def test_a_course_with_some_of_its_periods_keeps_those(problem, solved):
    kor = [x for x in solved["lessons"] if x["course_id"] == "C2-KOR"]
    kept = [x for x in solved["lessons"] if x not in kor[1:]]

    result = st.solve(problem, time_limit_seconds=0, initial_solution={"lessons": kept})

    assert set(slots({"lessons": kept})) <= set(slots(result))
    assert len(result["lessons"]) == len(solved["lessons"])


FIXED = ("C1-PE", 2, 3)  # the demo problem's fixed lesson


@pytest.mark.parametrize("mode", MODES)
def test_a_fixed_lesson_stays_when_the_seed_leaves_it_out(problem, solved, mode):
    kept = [x for x in solved["lessons"] if (x["course_id"], x["slot"]["day"], x["slot"]["period"]) != FIXED]

    result = st.solve(problem, mode=mode, time_limit_seconds=0, initial_solution={"lessons": kept})

    assert FIXED in slots(result)


@pytest.mark.parametrize("mode", MODES)
def test_a_seed_that_moves_a_fixed_lesson_is_refused(problem, solved, mode):
    seed = copy.deepcopy(solved)
    free_time = next(
        (day, period)
        for day in range(5)
        for period in range(1, 5)
        if not any(course == "C1-PE" and (d, p) == (day, period) for course, d, p in slots(solved))
    )
    fixed = next(x for x in seed["lessons"] if (x["course_id"], x["slot"]["day"], x["slot"]["period"]) == FIXED)
    fixed["slot"] = {"day": free_time[0], "period": free_time[1]}

    with pytest.raises(ValueError, match=r"\[C1-PE\] 3 slots besides its fixed ones, for 2 free lessons"):
        st.solve(problem, mode=mode, time_limit_seconds=0, initial_solution=seed)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize(
    "lessons, message",
    [
        (lessons_of([("nope", 0, 1)]), "unknown course: nope"),
        (lessons_of([("C1-KOR", 0, 1), ("C1-KOR", 0, 1)]), r"\[C1-KOR\] appears twice"),
        (lessons_of([("C1-KOR", 9, 1)]), r"\[C1-KOR\] slot not in the calendar"),
        (lessons_of([("C1-KOR", day, 2) for day in range(5)]), r"\[C1-KOR\] 5 slots .* for 4 free lessons"),
        ([{"course_id": "C1-KOR"}], "malformed lesson"),
    ],
)
def test_a_bad_seed_is_refused(problem, mode, lessons, message):
    with pytest.raises(ValueError, match=message):
        st.solve(problem, mode=mode, time_limit_seconds=0, initial_solution={"lessons": lessons})


def test_a_seed_with_no_lessons_is_refused(problem):
    with pytest.raises(ValueError, match="no lessons"):
        st.solve(problem, time_limit_seconds=0, initial_solution={"status": "FEASIBLE"})


def test_a_forbidden_time_is_scored_not_refused(problem):
    """Teacher Kim is away on Monday period 1: a violation the search can still repair."""
    result = st.solve(problem, time_limit_seconds=0, initial_solution={"lessons": lessons_of([("C1-KOR", 0, 1)])})

    assert ("C1-KOR", 0, 1) in slots(result)
    assert result["status"] == "INFEASIBLE"


def parallel_problem(problem: dict) -> dict:
    """C1's English and Korean run as one parallel group."""
    for course in problem["courses"]:
        if course["id"] in ("C1-KOR", "C1-ENG"):
            course["parallel_group"] = "C1-LANG"
    for course in problem["courses"]:
        if course["id"] == "C1-MATH":
            course["periods_per_week"] += 4  # the merge freed four periods; the grid stays full
            course["max_periods_per_day"] = 2
    return problem


def times_construction_avoids(problem: dict, course_id: str) -> list[tuple[int, int]]:
    """Seeding these proves the seed was read: construction would not land there by chance."""
    constructed = {(day, period) for c, day, period in slots(st.solve(problem, time_limit_seconds=0)) if c == course_id}
    return [(day, period) for day in range(5) for period in range(1, 5) if (day, period) not in constructed]


def test_parallel_sections_are_read_together(problem):
    problem = parallel_problem(problem)
    (day, period), (other_day, other_period), *_ = times_construction_avoids(problem, "C1-ENG")
    seed = lessons_of([("C1-KOR", day, period), ("C1-ENG", day, period), ("C1-ENG", other_day, other_period)])

    result = st.solve(problem, time_limit_seconds=0, initial_solution={"lessons": seed})

    both = {(course, *time) for course in ("C1-KOR", "C1-ENG") for time in ((day, period), (other_day, other_period))}
    assert both <= set(slots(result))


def test_a_section_given_only_on_its_second_course_is_not_lost(problem):
    problem = parallel_problem(problem)
    day, period = times_construction_avoids(problem, "C1-ENG")[0]

    result = st.solve(problem, time_limit_seconds=0, initial_solution={"lessons": lessons_of([("C1-ENG", day, period)])})

    assert {("C1-KOR", day, period), ("C1-ENG", day, period)} <= set(slots(result))


def test_parallel_sections_at_different_times_are_refused(problem):
    problem = parallel_problem(problem)
    seed = lessons_of([("C1-KOR", 3, 1), ("C1-ENG", 3, 2)])

    with pytest.raises(ValueError, match="parallel group C1-LANG"):
        st.solve(problem, time_limit_seconds=0, initial_solution={"lessons": seed})


def test_a_block_is_seeded_on_consecutive_periods(problem):
    course = next(c for c in problem["courses"] if c["id"] == "C1-MUS")
    course["block_periods"] = 2
    course["max_periods_per_day"] = 2
    seed = lessons_of([("C1-MUS", 3, 2), ("C1-MUS", 3, 3)])

    result = st.solve(problem, time_limit_seconds=0, initial_solution={"lessons": seed})

    assert {("C1-MUS", 3, 2), ("C1-MUS", 3, 3)} <= set(slots(result))
    assert not any("C1-MUS" in note for note in result["notes"])


def test_the_input_is_not_changed(problem, solved):
    seed = swap_days(solved, 0, 1)
    before = copy.deepcopy((problem, seed))

    st.solve(problem, time_limit_seconds=0.05, initial_solution=seed)

    assert (problem, seed) == before


def test_a_short_search_is_not_worse_than_its_seed(problem, solved):
    seed = swap_days(solved, 0, 1)
    start = st.solve(problem, time_limit_seconds=0, initial_solution=seed)

    improved = st.solve(problem, time_limit_seconds=0.3, initial_solution=seed)

    rank = {"FEASIBLE": 1, "INFEASIBLE": 0}
    assert rank[improved["status"]] >= rank[start["status"]]
    if improved["status"] == start["status"] == "FEASIBLE":
        assert improved["objective_value"] <= start["objective_value"]


def test_a_seed_with_a_hard_violation_can_be_resumed(problem):
    seed = st.solve(problem, time_limit_seconds=0, initial_solution={"lessons": lessons_of([("C1-KOR", 0, 1)])})
    assert seed["status"] == "INFEASIBLE"

    result = st.solve(problem, time_limit_seconds=2.0, initial_solution=seed, stop_at_first_feasible=True)

    assert result["status"] == "FEASIBLE"
