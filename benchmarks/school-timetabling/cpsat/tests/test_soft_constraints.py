"""Soft constraints: do the penalties the solver reports match the values counted by hand from the timetable?"""

from helpers import NO_WEIGHTS, course, make_problem, quick_solve, slot, teacher

from teacher_scheduling.domain.entities import Subject
from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.validate import compute_penalties, validate_timetable


def _assert_penalties_match_hand_count(problem, timetable):
    hand = compute_penalties(problem, timetable)
    for name, count in timetable.penalties.items():
        assert count == hand[name], f"{name}: solver {count} != hand count {hand[name]}"


def test_forced_gap_matches_hand_count():
    """Fixed to periods 1 and 5, the 3 cells between count as teacher/class gaps."""
    problem = make_problem(
        [course(periods_per_week=2, max_periods_per_day=2, fixed_slots=frozenset({slot(0, 1), slot(0, 5)}))],
        [teacher()],
        days=1,
        periods=5,
    )
    weights = NO_WEIGHTS.model_copy(update={"teacher_gap": 1, "class_gap": 1})
    timetable = quick_solve(problem, weights=weights)
    assert timetable.status.has_solution
    assert validate_timetable(problem, timetable) == []
    assert timetable.penalties["teacher_gap"] == 3
    assert timetable.penalties["class_gap"] == 3
    _assert_penalties_match_hand_count(problem, timetable)


def test_gap_minimized_to_zero_when_free():
    problem = make_problem([course(periods_per_week=3, max_periods_per_day=3)], [teacher()], days=1, periods=5)
    timetable = quick_solve(problem, weights=NO_WEIGHTS.model_copy(update={"teacher_gap": 1}))
    assert timetable.status.has_solution
    assert timetable.penalties["teacher_gap"] == 0
    _assert_penalties_match_hand_count(problem, timetable)


def test_workday_penalty_packs_lessons_into_one_day():
    problem = make_problem([course(periods_per_week=2, max_periods_per_day=2)], [teacher()], days=2, periods=4)
    timetable = quick_solve(problem, weights=NO_WEIGHTS.model_copy(update={"teacher_workday": 1}))
    assert timetable.status.has_solution
    assert timetable.penalties["teacher_workday"] == 1  # packed into one day instead of two
    assert len({lesson.slot.day for lesson in timetable.lessons}) == 1
    _assert_penalties_match_hand_count(problem, timetable)


def test_preferred_free_day_respected_when_possible():
    problem = make_problem(
        [course(periods_per_week=2, max_periods_per_day=2)],
        [teacher(preferred_free_days=frozenset({Weekday.TUE}))],
        days=2,
        periods=4,
    )
    timetable = quick_solve(problem, weights=NO_WEIGHTS.model_copy(update={"teacher_free_day": 1}))
    assert timetable.status.has_solution
    assert timetable.penalties["teacher_free_day"] == 0
    assert {lesson.slot.day for lesson in timetable.lessons} == {Weekday.MON}
    _assert_penalties_match_hand_count(problem, timetable)


def test_forced_free_day_violation_counted():
    problem = make_problem(
        [course(periods_per_week=1, fixed_slots=frozenset({slot(1, 1)}))],
        [teacher(preferred_free_days=frozenset({Weekday.TUE}))],
        days=2,
        periods=4,
    )
    timetable = quick_solve(problem, weights=NO_WEIGHTS.model_copy(update={"teacher_free_day": 1}))
    assert timetable.status.has_solution
    assert timetable.penalties["teacher_free_day"] == 1
    _assert_penalties_match_hand_count(problem, timetable)


def test_period_preferences_steer_placement():
    subjects = [Subject(id="SUB", name="SUB", preferred_periods=frozenset({2}), avoided_periods=frozenset({1}))]
    weights = NO_WEIGHTS.model_copy(update={"avoided_period": 1, "non_preferred_period": 1})

    problem = make_problem([course(periods_per_week=1)], [teacher()], subjects=subjects, days=1, periods=3)
    timetable = quick_solve(problem, weights=weights)
    assert timetable.status.has_solution
    assert timetable.penalties["avoided_period"] == 0
    assert timetable.penalties["non_preferred_period"] == 0
    assert [lesson.slot.period for lesson in timetable.lessons] == [2]

    # Fixed to period 1, both penalties are caught.
    forced = make_problem(
        [course(periods_per_week=1, fixed_slots=frozenset({slot(0, 1)}))],
        [teacher()],
        subjects=subjects,
        days=1,
        periods=3,
    )
    forced_timetable = quick_solve(forced, weights=weights)
    assert forced_timetable.penalties["avoided_period"] == 1
    assert forced_timetable.penalties["non_preferred_period"] == 1
    _assert_penalties_match_hand_count(forced, forced_timetable)
