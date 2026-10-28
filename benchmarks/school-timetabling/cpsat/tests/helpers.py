"""Small problem builders for the tests.

Subjects and classes are generated from the course definitions, so each test
can spell out just the constraint it checks.
"""

from teacher_scheduling.domain.entities import ClassGroup, Course, Room, Subject, Teacher, TimeSlot
from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.domain.problem import SchoolCalendar, TimetableProblem
from teacher_scheduling.solver.config import ObjectiveWeights, SolverConfig
from teacher_scheduling.solver.solver import solve

# Hard-constraint tests solve as a pure satisfaction problem, without an objective.
NO_WEIGHTS = ObjectiveWeights(
    teacher_gap=0,
    teacher_workday=0,
    teacher_free_day=0,
    class_gap=0,
    avoided_period=0,
    non_preferred_period=0,
)


def slot(day: int, period: int) -> TimeSlot:
    return TimeSlot(day=Weekday(day), period=period)


def teacher(teacher_id: str = "T1", **kwargs) -> Teacher:
    return Teacher(id=teacher_id, name=teacher_id, **kwargs)


def course(course_id: str = "K1", **kwargs) -> Course:
    kwargs.setdefault("class_group_id", "C1")
    kwargs.setdefault("subject_id", "SUB")
    kwargs.setdefault("teacher_ids", ("T1",))
    kwargs.setdefault("periods_per_week", 1)
    return Course(id=course_id, **kwargs)


def make_problem(
    courses: list[Course],
    teachers: list[Teacher],
    *,
    subjects: list[Subject] | None = None,
    class_groups: list[ClassGroup] | None = None,
    rooms: tuple[Room, ...] = (),
    days: int = 1,
    periods: int = 4,
) -> TimetableProblem:
    if subjects is None:
        subjects = [Subject(id=sid, name=sid) for sid in sorted({c.subject_id for c in courses})]
    if class_groups is None:
        class_ids = sorted({c.class_group_id for c in courses})
        class_groups = [ClassGroup(id=cid, name=cid, grade=1) for cid in class_ids]
    return TimetableProblem(
        calendar=SchoolCalendar.uniform([Weekday(d) for d in range(days)], periods),
        subjects=tuple(subjects),
        teachers=tuple(teachers),
        class_groups=tuple(class_groups),
        rooms=rooms,
        courses=tuple(courses),
    )


def quick_solve(problem: TimetableProblem, *, weights: ObjectiveWeights = NO_WEIGHTS, time_limit: float = 5.0):
    config = SolverConfig(time_limit_seconds=time_limit, num_workers=4, weights=weights)
    return solve(problem, config)
