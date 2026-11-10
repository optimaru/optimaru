"""solve_partial(): periods that cannot be placed stay unassigned instead of making the timetable infeasible."""

from helpers import course, make_problem, teacher

from teacher_scheduling.partial import solve_partial, unassigned_courses
from teacher_scheduling.solver.config import SolverConfig


def test_partial_solve_leaves_unassigned_instead_of_infeasible():
    """Unfillable periods stay unassigned and the rest is placed."""
    # One day of 2 cells, 5 periods required: 3 stay unassigned.
    korean = course("K1", subject_id="Korean", periods_per_week=5, max_periods_per_day=5)
    problem = make_problem([korean], [teacher("T1")], days=1, periods=2)
    timetable = solve_partial(problem, SolverConfig(time_limit_seconds=10))
    assert timetable.status.has_solution, "partial timetabling must not be INFEASIBLE"
    assert len(timetable.lessons) == 2
    assert [(u.subject_id, u.missing) for u in unassigned_courses(problem, timetable)] == [("Korean", 3)]


def test_parallel_group_is_not_penalized_twice_for_one_missing_period():
    """Parallel sections always move together (parallel_groups_cost), so the unassigned penalty counts once
    per group."""
    # One day of 2 cells, the two parallel courses need 4 each: 2 periods unassigned per group.
    courses = [
        course(
            "A", subject_id="Tech", teacher_ids=("T1",), periods_per_week=4, max_periods_per_day=4, parallel_group="G"
        ),
        course(
            "B", subject_id="HomeEc", teacher_ids=("T2",), periods_per_week=4, max_periods_per_day=4, parallel_group="G"
        ),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2")], days=1, periods=2)
    timetable = solve_partial(problem, SolverConfig(time_limit_seconds=10))

    assert len(timetable.lessons) == 4, "the two parallel courses share the 2 cells"
    missing = {u.subject_id: u.missing for u in unassigned_courses(problem, timetable)}
    assert missing == {"Tech": 2, "HomeEc": 2}, "the unassigned list still shows each course"
    assert timetable.penalties["unassigned_lessons_cost"] == 2, "must not be multiplied by the group size"
