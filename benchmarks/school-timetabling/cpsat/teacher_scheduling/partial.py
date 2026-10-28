"""Partial timetabling that allows unassigned lessons.

Comcigan leaves unfillable periods in an "unassigned list" and shows the rest
placed. In the default configuration H1 (weekly periods met) is hard, so a
single cell that does not fit makes the whole problem INFEASIBLE — and the user
cannot see why.

All this does is swap the H1 constraint for its relaxed mode —
`WeeklyPeriodCounts(hard=False)` turns "exactly" into "at most" and the
unfilled periods into a penalty. Its weight is far above the sum of the other
soft terms, so every period that can be filled is filled and only the truly
impossible ones stay unassigned.
"""

from __future__ import annotations

from dataclasses import dataclass

from teacher_scheduling.domain.problem import TimetableProblem
from teacher_scheduling.domain.solution import Timetable
from teacher_scheduling.solver.config import SolverConfig
from teacher_scheduling.solver.registry import (
    UNASSIGNED_WEIGHT,
    WeeklyPeriodCounts,
    default_constraints,
    merge,
)


@dataclass(frozen=True)
class UnassignedCourse:
    """One course with unassigned periods left."""

    course_id: str
    class_group_id: str
    class_group_name: str
    subject_id: str
    teacher_ids: tuple[str, ...]
    teacher_names: tuple[str, ...]
    required: int
    scheduled: int

    @property
    def missing(self) -> int:
        return self.required - self.scheduled


def solve_partial(problem: TimetableProblem, config: SolverConfig | None = None) -> Timetable:
    """Solves allowing unassigned lessons. Returns a partial solution with every fillable period filled.

    Unassigned periods are reported as `Timetable.penalties["weekly_period_counts"]`.
    The penalty size can be set in `config.weights` under the same name.
    """
    from teacher_scheduling.solver.solver import solve

    config = config or SolverConfig()
    weight = config.weights.model_dump().get(WeeklyPeriodCounts.name, UNASSIGNED_WEIGHT)
    relaxed = WeeklyPeriodCounts(hard=False, weight=weight)
    return solve(problem, config, constraints=merge(default_constraints(config.weights), relaxed))


def unassigned_courses(problem: TimetableProblem, timetable: Timetable) -> list[UnassignedCourse]:
    """Courses whose periods could not all be filled — Comcigan's "unassigned list"."""
    scheduled: dict[str, int] = {}
    for lesson in timetable.lessons:
        scheduled[lesson.course_id] = scheduled.get(lesson.course_id, 0) + 1

    result = []
    for course in problem.courses:
        done = scheduled.get(course.id, 0)
        if done >= course.periods_per_week:
            continue
        result.append(
            UnassignedCourse(
                course_id=course.id,
                class_group_id=course.class_group_id,
                class_group_name=problem.class_group_by_id[course.class_group_id].name,
                subject_id=course.subject_id,
                teacher_ids=course.teacher_ids,
                teacher_names=tuple(problem.teacher_by_id[t].name for t in course.teacher_ids),
                required=course.periods_per_week,
                scheduled=done,
            )
        )
    return sorted(result, key=lambda item: (-item.missing, item.course_id))


__all__ = [
    "UNASSIGNED_WEIGHT",
    "UnassignedCourse",
    "solve_partial",
    "unassigned_courses",
]
