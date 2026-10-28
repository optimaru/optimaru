"""Checks the hard constraints H1–H10 one by one on small instances."""

from collections import Counter

import pytest
from helpers import course, make_problem, quick_solve, slot, teacher

from teacher_scheduling.domain.entities import ClassGroup, Room, Subject
from teacher_scheduling.domain.solution import SolveStatus
from teacher_scheduling.validate import validate_timetable


def _solved_and_valid(problem, timetable):
    assert timetable.status.has_solution, timetable.status
    assert validate_timetable(problem, timetable) == []


# ---------------------------------------------------------------- H1 weekly periods


def test_weekly_period_count():
    problem = make_problem([course(periods_per_week=3, max_periods_per_day=3)], [teacher()], days=1, periods=5)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert len(timetable.lessons) == 3


# ---------------------------------------------------------------- H2 fixed time


def test_fixed_slot_is_scheduled():
    problem = make_problem(
        [course(periods_per_week=2, max_periods_per_day=2, fixed_slots=frozenset({slot(0, 3)}))],
        [teacher()],
        days=1,
        periods=4,
    )
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert slot(0, 3) in {lesson.slot for lesson in timetable.lessons}


def test_fixed_slot_conflicting_with_unavailable_raises():
    problem = make_problem(
        [course(periods_per_week=1, fixed_slots=frozenset({slot(0, 1)}))],
        [teacher(unavailable=frozenset({slot(0, 1)}))],
        days=1,
        periods=3,
    )
    with pytest.raises(ValueError, match="fixed time"):
        quick_solve(problem)


# ---------------------------------------------------------------- forbidden/unavailable times


def test_forbidden_and_unavailable_slots_are_avoided():
    problem = make_problem(
        [course(periods_per_week=2, max_periods_per_day=2, forbidden_slots=frozenset({slot(0, 2)}))],
        [teacher(unavailable=frozenset({slot(0, 4)}))],
        class_groups=[ClassGroup(id="C1", name="C1", grade=1, unavailable=frozenset({slot(0, 5)}))],
        days=1,
        periods=5,
    )
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert {lesson.slot.period for lesson in timetable.lessons} == {1, 3}


# ---------------------------------------------------------------- H3/H4 no clashes


def test_class_conflicts_forbidden():
    """A class's two courses, 4 periods in all, fill exactly 4 periods without overlap."""
    courses = [
        course("K1", subject_id="S1", teacher_ids=("T1",), periods_per_week=2, max_periods_per_day=2),
        course("K2", subject_id="S2", teacher_ids=("T2",), periods_per_week=2, max_periods_per_day=2),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2")], days=1, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert len({lesson.slot for lesson in timetable.lessons}) == 4


def test_teacher_conflicts_forbidden():
    """A teacher teaching two classes for 4 periods needs all-different slots."""
    courses = [
        course("K1", class_group_id="C1", periods_per_week=2, max_periods_per_day=2),
        course("K2", class_group_id="C2", periods_per_week=2, max_periods_per_day=2),
    ]
    problem = make_problem(courses, [teacher()], days=1, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert len({lesson.slot for lesson in timetable.lessons}) == 4


# ---------------------------------------------------------------- H5 parallel sections


def test_parallel_group_runs_simultaneously():
    courses = [
        course("A", teacher_ids=("T1",), periods_per_week=2, max_periods_per_day=2, parallel_group="G"),
        course("B", teacher_ids=("T2",), periods_per_week=2, max_periods_per_day=2, parallel_group="G"),
        course("K3", subject_id="S3", teacher_ids=("T3",), periods_per_week=2, max_periods_per_day=2),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2"), teacher("T3")], days=1, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    slots_a = {lesson.slot for lesson in timetable.lessons if lesson.course_id == "A"}
    slots_b = {lesson.slot for lesson in timetable.lessons if lesson.course_id == "B"}
    slots_rest = {lesson.slot for lesson in timetable.lessons if lesson.course_id == "K3"}
    assert slots_a == slots_b
    assert slots_a.isdisjoint(slots_rest)  # a parallel group takes one class cell too


# ---------------------------------------------------------------- H6 special-room capacity


def test_room_capacity_respected():
    """Two classes sharing one science lab cannot overlap in time."""
    subjects = [Subject(id="SCI", name="Science", required_room_type="science lab")]
    rooms = (Room(id="R1", name="Science lab 1", room_type="science lab"),)
    courses = [
        course(
            "K1", class_group_id="C1", subject_id="SCI", periods_per_week=2, max_periods_per_day=2, teacher_ids=("T1",)
        ),
        course(
            "K2", class_group_id="C2", subject_id="SCI", periods_per_week=2, max_periods_per_day=2, teacher_ids=("T2",)
        ),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2")], subjects=subjects, rooms=rooms, days=1, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert len({lesson.slot for lesson in timetable.lessons}) == 4
    assert all(lesson.room_id == "R1" for lesson in timetable.lessons)


def test_two_rooms_shared_in_same_slot():
    """With 2 rooms two classes can use them at the same time, and the rooms do not overlap."""
    subjects = [Subject(id="SCI", name="Science", required_room_type="science lab")]
    rooms = (
        Room(id="R1", name="Science lab 1", room_type="science lab"),
        Room(id="R2", name="Science lab 2", room_type="science lab"),
    )
    courses = [
        course(
            "K1", class_group_id="C1", subject_id="SCI", periods_per_week=2, max_periods_per_day=2, teacher_ids=("T1",)
        ),
        course(
            "K2", class_group_id="C2", subject_id="SCI", periods_per_week=2, max_periods_per_day=2, teacher_ids=("T2",)
        ),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2")], subjects=subjects, rooms=rooms, days=1, periods=2)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)  # room double use / type errors are caught by the validator
    used = Counter((lesson.slot, lesson.room_id) for lesson in timetable.lessons)
    assert all(count == 1 for count in used.values())


# ---------------------------------------------------------------- H7 daily cap per course


def test_daily_course_limit():
    problem = make_problem([course(periods_per_week=4, max_periods_per_day=2)], [teacher()], days=2, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    per_day = Counter(lesson.slot.day for lesson in timetable.lessons)
    assert sorted(per_day.values()) == [2, 2]


# ---------------------------------------------------------------- H8 block time


def test_block_periods_are_consecutive():
    problem = make_problem([course(periods_per_week=3, block_periods=3)], [teacher()], days=2, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)  # block continuity is checked by the validator's H8 check
    per_day: dict = {}
    for lesson in timetable.lessons:
        per_day.setdefault(lesson.slot.day, []).append(lesson.slot.period)
    (periods,) = per_day.values()  # a 3-period block lands on one day
    periods.sort()
    assert periods == list(range(periods[0], periods[0] + 3))


def test_block_without_room_raises_value_error():
    """Without any consecutive cells for the block, a clear input error is raised."""
    problem = make_problem(
        [course(periods_per_week=2, block_periods=2, forbidden_slots=frozenset({slot(0, 2)}))],
        [teacher()],
        days=1,
        periods=3,
    )
    with pytest.raises(ValueError, match="block"):
        quick_solve(problem)


# ---------------------------------------------------------------- H9 teacher daily/weekly caps


def test_teacher_weekly_max():
    def build(limit):
        courses = [
            course("K1", class_group_id="C1", periods_per_week=2, max_periods_per_day=2),
            course("K2", class_group_id="C2", periods_per_week=2, max_periods_per_day=2),
        ]
        return make_problem(courses, [teacher(max_periods_per_week=limit)], days=2, periods=4)

    infeasible = quick_solve(build(3))
    assert infeasible.status == SolveStatus.INFEASIBLE
    assert infeasible.lessons == ()

    problem = build(4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)


def test_teacher_daily_max():
    courses = [
        course("K1", class_group_id="C1", periods_per_week=2, max_periods_per_day=2),
        course("K2", class_group_id="C2", periods_per_week=2, max_periods_per_day=2),
    ]
    problem = make_problem(courses, [teacher(max_periods_per_day=2)], days=2, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    per_day = Counter(lesson.slot.day for lesson in timetable.lessons)
    assert sorted(per_day.values()) == [2, 2]


# ---------------------------------------------------------------- H10 consecutive-lessons limit


def test_teacher_consecutive_limit():
    problem = make_problem(
        [course(periods_per_week=3, max_periods_per_day=3)],
        [teacher(max_consecutive_periods=2)],
        days=1,
        periods=5,
    )
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)  # the validator's H10 check catches a consecutive-cap violation
    periods = sorted(lesson.slot.period for lesson in timetable.lessons)
    assert periods[2] - periods[0] != 2  # the three periods are not back to back


# ---------------------------------------------------------------- INFEASIBLE


def test_overfull_class_is_infeasible():
    """More periods (4) than the class's available cells (3) cannot be placed."""
    courses = [
        course("K1", subject_id="S1", teacher_ids=("T1",), periods_per_week=2, max_periods_per_day=2),
        course("K2", subject_id="S2", teacher_ids=("T2",), periods_per_week=2, max_periods_per_day=2),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2")], days=1, periods=3)
    timetable = quick_solve(problem)
    assert timetable.status == SolveStatus.INFEASIBLE
    assert timetable.lessons == ()
