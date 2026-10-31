"""The basic entities that make up a timetabling problem.

Terms follow the practice of Korean middle/high-school timetabling (Comcigan and the like).
- Class (ClassGroup): a group of students that moves together, e.g. grade 1 class 3
- Course: a lesson that is the unit of assignment, e.g. "grade 1 class 3 / Math / Kim / 4 periods a week"
- Block: periods that must be joined as consecutive periods
- Parallel group (parallel_group): a class split into groups taught by different teachers at the same time
"""

from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field, model_validator

from teacher_scheduling.domain.enums import Weekday


class TimeSlot(BaseModel):
    """One lesson cell, given by weekday + period."""

    model_config = ConfigDict(frozen=True)

    day: Weekday
    period: int = Field(ge=1, description="Period number, starting at period 1")

    def __str__(self) -> str:
        return f"{self.day.label}{self.period}"


class Subject(BaseModel):
    """A subject. Time preferences only enter as soft constraints."""

    id: str
    name: str
    required_room_type: str | None = Field(default=None, description="Special-room type needed. None uses the class's own room")
    preferred_periods: frozenset[int] = Field(default=frozenset(), description="Preferred periods. Empty means no preference")
    avoided_periods: frozenset[int] = Field(default=frozenset(), description="Avoided periods (e.g. PE in period 1)")


class Teacher(BaseModel):
    """A teacher. Personal constraints (trips, other schools, childcare hours) go into `unavailable`."""

    id: str
    name: str
    subject_ids: frozenset[str] = Field(default=frozenset(), description="Subjects the teacher can teach. Empty means unrestricted")
    unavailable: frozenset[TimeSlot] = Field(default=frozenset(), description="Times no lesson can be assigned")
    max_periods_per_week: int | None = Field(default=None, ge=0)
    max_periods_per_day: int = Field(default=6, ge=1)
    max_consecutive_periods: int = Field(default=4, ge=1, description="Maximum periods in a row without a break")
    preferred_free_days: frozenset[Weekday] = Field(default=frozenset(), description="Requested free weekdays (soft)")
    homeroom_class_id: str | None = Field(default=None, description="Id of the class this teacher is homeroom teacher of")


class ClassGroup(BaseModel):
    """A class."""

    id: str
    name: str
    grade: int = Field(ge=1)
    unavailable: frozenset[TimeSlot] = Field(default=frozenset(), description="Times the class itself cannot have lessons")


class Room(BaseModel):
    """A classroom / special room."""

    id: str
    name: str
    room_type: str


class Course(BaseModel):
    """A unit of assignment. The goal is to place as many cells as its weekly periods."""

    id: str
    class_group_id: str
    subject_id: str
    teacher_ids: tuple[str, ...] = Field(min_length=1, description="Two or more means team teaching")
    periods_per_week: int = Field(ge=1)
    block_periods: int = Field(default=0, ge=0, description="Periods joined into a block. 0 or 1 means no block")
    max_periods_per_day: int = Field(default=1, ge=1, description="Maximum periods of this course on one day")
    fixed_slots: frozenset[TimeSlot] = Field(default=frozenset(), description="Cells the course must occupy")
    forbidden_slots: frozenset[TimeSlot] = Field(default=frozenset(), description="Times blocked for this course only")
    parallel_group: str | None = Field(default=None, description="Courses with the same value run at the same time (parallel sections)")

    @model_validator(mode="after")
    def _check_consistency(self) -> Course:
        if self.block_periods > self.periods_per_week:
            raise ValueError(f"[{self.id}] block_periods cannot exceed periods_per_week")
        if len(self.fixed_slots) > self.periods_per_week:
            raise ValueError(f"[{self.id}] more fixed_slots than periods_per_week")
        if overlap := self.fixed_slots & self.forbidden_slots:
            raise ValueError(f"[{self.id}] fixed and forbidden overlap: {sorted(map(str, overlap))}")
        return self

    @property
    def has_block(self) -> bool:
        return self.block_periods >= 2

    @property
    def effective_max_periods_per_day(self) -> int:
        """With a block, that day must allow at least the block size."""
        return max(self.max_periods_per_day, self.block_periods)

    @property
    def occupancy_key(self) -> str:
        """The unit grouped as one from the class-occupancy view. A parallel group uses one cell as a whole."""
        return self.parallel_group or self.id
