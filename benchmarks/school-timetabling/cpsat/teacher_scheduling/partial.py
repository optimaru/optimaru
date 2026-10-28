"""미배정을 허용하는 부분 편성.

컴시간은 못 채운 시수를 [미배정 목록] 으로 남기고 나머지는 배정한 채로 보여준다.
기본 구성은 H1(주당 시수 충족)이 하드라서 한 칸이라도 안 들어가면 통째로
INFEASIBLE 이 되는데, 그러면 "무엇 때문에 안 되는지" 를 사용자가 알 수 없다.

여기서 하는 일은 H1 제약 하나를 완화 모드로 갈아끼우는 것뿐이다 —
`WeeklyPeriodCounts(hard=False)` 가 '정확히' 를 '이하' 로 바꾸고 못 채운 시수를
벌점으로 돌린다. 가중치가 다른 소프트 항목 합보다 훨씬 크므로 채울 수 있는
시수는 전부 채우고, 정말 불가능한 것만 미배정으로 남는다.
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
    """미배정이 남은 강좌 한 건."""

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
    """미배정을 허용해 푼다. 채울 수 있는 시수는 모두 채운 부분해를 돌려준다.

    미배정 시수는 `Timetable.penalties["weekly_period_counts"]` 로 보고된다.
    벌점 크기는 `config.weights` 에 같은 이름으로 지정할 수 있다.
    """
    from teacher_scheduling.solver.solver import solve

    config = config or SolverConfig()
    weight = config.weights.model_dump().get(WeeklyPeriodCounts.name, UNASSIGNED_WEIGHT)
    relaxed = WeeklyPeriodCounts(hard=False, weight=weight)
    return solve(problem, config, constraints=merge(default_constraints(config.weights), relaxed))


def unassigned_courses(problem: TimetableProblem, timetable: Timetable) -> list[UnassignedCourse]:
    """컴시간 [미배정 목록] 에 해당하는, 시수를 못 채운 강좌 목록."""
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
