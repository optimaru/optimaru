"""편성 결과 정의."""

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
    """시간표에 꽂힌 한 칸."""

    course_id: str
    class_group_id: str
    subject_id: str
    teacher_ids: tuple[str, ...]
    slot: TimeSlot
    room_id: str | None = None


class Timetable(BaseModel):
    """편성 결과 + 품질 지표."""

    status: SolveStatus
    lessons: tuple[ScheduledLesson, ...] = ()
    objective_value: float | None = None
    best_objective_bound: float | None = None
    wall_time_seconds: float = 0.0
    penalties: dict[str, int] = Field(default_factory=dict, description="소프트 제약별 위반량(가중치 적용 전)")
    notes: list[str] = Field(default_factory=list, description="백엔드가 남기는 안내 (예: 변환 과정에서 빠진 제약)")

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
