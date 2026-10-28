"""The demo school sample: solves within 10 seconds and the result respects every hard constraint."""

import pytest

from teacher_scheduling.report import render_report
from teacher_scheduling.sample import build_sample_problem
from teacher_scheduling.solver.config import SolverConfig
from teacher_scheduling.solver.solver import solve
from teacher_scheduling.validate import compute_penalties, validate_timetable


@pytest.fixture(scope="module")
def sample_result():
    problem = build_sample_problem()
    timetable = solve(problem, SolverConfig(time_limit_seconds=10.0))
    return problem, timetable


def test_sample_hours_fill_calendar():
    """Each class's weekly periods (a parallel group counted once) equal the available slots exactly."""
    problem = build_sample_problem()
    total_slots = len(problem.calendar.slots)
    for class_group in problem.class_groups:
        by_occupancy = {c.occupancy_key: c.periods_per_week for c in problem.courses_by_class_group[class_group.id]}
        assert sum(by_occupancy.values()) == total_slots, class_group.id


def test_sample_solves_within_10s(sample_result):
    _problem, timetable = sample_result
    assert timetable.status.has_solution, timetable.status
    assert timetable.wall_time_seconds < 12.0  # 10 s limit + slack to finish


def test_sample_respects_all_hard_constraints(sample_result):
    problem, timetable = sample_result
    assert validate_timetable(problem, timetable) == []


def test_sample_penalties_match_hand_count(sample_result):
    problem, timetable = sample_result
    hand = compute_penalties(problem, timetable)
    for name, count in timetable.penalties.items():
        assert count == hand[name], f"{name}: solver {count} != hand count {hand[name]}"


def test_sample_special_rooms_assigned(sample_result):
    problem, timetable = sample_result
    for lesson in timetable.lessons:
        required = problem.required_room_type(problem.course_by_id[lesson.course_id])
        assert (lesson.room_id is not None) == (required is not None), lesson.course_id


def test_report_renders(sample_result):
    problem, timetable = sample_result
    report = render_report(problem, timetable, SolverConfig().weights)
    assert "status" in report
    assert "[1-1 timetable]" in report
    assert "[Kim (Korean) timetable]" in report
