"""데모 학교 샘플: 10초 안에 풀리고, 결과가 하드 제약을 전부 지키는지."""

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
    """학급마다 주당 시수 합(분반은 한 번만)이 가용 슬롯 수와 정확히 같다."""
    problem = build_sample_problem()
    total_slots = len(problem.calendar.slots)
    for class_group in problem.class_groups:
        by_occupancy = {c.occupancy_key: c.periods_per_week for c in problem.courses_by_class_group[class_group.id]}
        assert sum(by_occupancy.values()) == total_slots, class_group.id


def test_sample_solves_within_10s(sample_result):
    _problem, timetable = sample_result
    assert timetable.status.has_solution, timetable.status
    assert timetable.wall_time_seconds < 12.0  # 제한 10초 + 마무리 여유


def test_sample_respects_all_hard_constraints(sample_result):
    problem, timetable = sample_result
    assert validate_timetable(problem, timetable) == []


def test_sample_penalties_match_hand_count(sample_result):
    problem, timetable = sample_result
    hand = compute_penalties(problem, timetable)
    for name, count in timetable.penalties.items():
        assert count == hand[name], f"{name}: 솔버 {count} != 손계산 {hand[name]}"


def test_sample_special_rooms_assigned(sample_result):
    problem, timetable = sample_result
    for lesson in timetable.lessons:
        required = problem.required_room_type(problem.course_by_id[lesson.course_id])
        assert (lesson.room_id is not None) == (required is not None), lesson.course_id


def test_report_renders(sample_result):
    problem, timetable = sample_result
    report = render_report(problem, timetable, SolverConfig().weights)
    assert "상태" in report
    assert "[1-1 시간표]" in report
    assert "[김국어 시간표]" in report
