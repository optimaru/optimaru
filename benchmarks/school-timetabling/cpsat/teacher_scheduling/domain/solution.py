"""Definition of the timetabling result."""

from collections import defaultdict
from enum import StrEnum

from pydantic import BaseModel, Field

from teacher_scheduling.domain.entities import TimeSlot


class SolveStatus(StrEnum):
    OPTIMAL = "OPTIMAL"
    FEASIBLE = "FEASIBLE"
    INFEASIBLE = "INFEASIBLE"
    MODEL_INVALID = "MODEL_INVALID"
    UNKNOWN = "UNKNOWN"

    @property
    def has_solution(self) -> bool:
        return self in (SolveStatus.OPTIMAL, SolveStatus.FEASIBLE)


class ScheduledLesson(BaseModel):
    """One cell placed in the timetable."""

    course_id: str
    class_group_id: str
    subject_id: str
    teacher_ids: tuple[str, ...]
    slot: TimeSlot
    room_id: str | None = None


class Timetable(BaseModel):
    """The timetabling result + quality measures."""

    status: SolveStatus
    lessons: tuple[ScheduledLesson, ...] = ()
    objective_value: float | None = None
    best_objective_bound: float | None = None
    wall_time_seconds: float = 0.0
    penalties: dict[str, int] = Field(default_factory=dict, description="Violation amount per soft constraint (before weighting)")
    notes: list[str] = Field(default_factory=list, description="Notes left by the backend (e.g. constraints dropped in a conversion)")

    @property
    def is_solved(self) -> bool:
        return self.status.has_solution

    def by_class_group(self) -> dict[str, dict[TimeSlot, list[ScheduledLesson]]]:
        result: dict[str, dict[TimeSlot, list[ScheduledLesson]]] = defaultdict(lambda: defaultdict(list))
        for lesson in self.lessons:
            result[lesson.class_group_id][lesson.slot].append(lesson)
        return result

    def by_teacher(self) -> dict[str, dict[TimeSlot, list[ScheduledLesson]]]:
        result: dict[str, dict[TimeSlot, list[ScheduledLesson]]] = defaultdict(lambda: defaultdict(list))
        for lesson in self.lessons:
            for teacher_id in lesson.teacher_ids:
                result[teacher_id][lesson.slot].append(lesson)
        return result
