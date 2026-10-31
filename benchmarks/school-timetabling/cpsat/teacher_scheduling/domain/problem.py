"""Input definition of the timetabling problem."""

from __future__ import annotations

from collections import defaultdict
from functools import cached_property

from pydantic import BaseModel, Field, model_validator

from teacher_scheduling.domain.entities import ClassGroup, Course, Room, Subject, Teacher, TimeSlot
from teacher_scheduling.domain.enums import Weekday


class SchoolCalendar(BaseModel):
    """Periods per weekday. Can differ by weekday, e.g. only 6 periods on Friday."""

    periods_per_day: dict[Weekday, int] = Field(min_length=1)

    @classmethod
    def uniform(cls, days: list[Weekday], periods: int) -> SchoolCalendar:
        return cls(periods_per_day={day: periods for day in days})

    @property
    def days(self) -> list[Weekday]:
        return sorted(self.periods_per_day)

    @property
    def max_periods(self) -> int:
        return max(self.periods_per_day.values())

    @cached_property
    def slots(self) -> tuple[TimeSlot, ...]:
        return tuple(
            TimeSlot(day=day, period=period) for day in self.days for period in range(1, self.periods_per_day[day] + 1)
        )

    def slots_of(self, day: Weekday) -> tuple[TimeSlot, ...]:
        return tuple(TimeSlot(day=day, period=p) for p in range(1, self.periods_per_day[day] + 1))

    def contains(self, slot: TimeSlot) -> bool:
        return slot.period <= self.periods_per_day.get(slot.day, 0)


class TimetableProblem(BaseModel):
    """Every input the scheduling needs, bundled."""

    calendar: SchoolCalendar
    subjects: tuple[Subject, ...]
    teachers: tuple[Teacher, ...]
    class_groups: tuple[ClassGroup, ...]
    rooms: tuple[Room, ...] = ()
    courses: tuple[Course, ...]

    @cached_property
    def subject_by_id(self) -> dict[str, Subject]:
        return {s.id: s for s in self.subjects}

    @cached_property
    def teacher_by_id(self) -> dict[str, Teacher]:
        return {t.id: t for t in self.teachers}

    @cached_property
    def class_group_by_id(self) -> dict[str, ClassGroup]:
        return {c.id: c for c in self.class_groups}

    @cached_property
    def course_by_id(self) -> dict[str, Course]:
        return {c.id: c for c in self.courses}

    @cached_property
    def rooms_by_type(self) -> dict[str, tuple[Room, ...]]:
        grouped: dict[str, list[Room]] = defaultdict(list)
        for room in self.rooms:
            grouped[room.room_type].append(room)
        return {room_type: tuple(rooms) for room_type, rooms in grouped.items()}

    @cached_property
    def courses_by_class_group(self) -> dict[str, tuple[Course, ...]]:
        grouped: dict[str, list[Course]] = defaultdict(list)
        for course in self.courses:
            grouped[course.class_group_id].append(course)
        return {class_id: tuple(courses) for class_id, courses in grouped.items()}

    @cached_property
    def courses_by_parallel_group(self) -> dict[str, tuple[Course, ...]]:
        grouped: dict[str, list[Course]] = defaultdict(list)
        for course in self.courses:
            if course.parallel_group is not None:
                grouped[course.parallel_group].append(course)
        return {group: tuple(courses) for group, courses in grouped.items()}

    def required_room_type(self, course: Course) -> str | None:
        return self.subject_by_id[course.subject_id].required_room_type

    @model_validator(mode="after")
    def _check_references(self) -> TimetableProblem:
        errors: list[str] = []
        errors.extend(self._check_duplicate_ids())
        errors.extend(self._check_course_references())
        errors.extend(self._check_parallel_groups())
        errors.extend(self._check_slot_validity())
        if errors:
            raise ValueError("invalid input data:\n- " + "\n- ".join(errors))
        return self

    def _check_duplicate_ids(self) -> list[str]:
        errors = []
        for label, ids in (
            ("subject", [s.id for s in self.subjects]),
            ("teacher", [t.id for t in self.teachers]),
            ("class", [c.id for c in self.class_groups]),
            ("room", [r.id for r in self.rooms]),
            ("course", [c.id for c in self.courses]),
        ):
            duplicates = {i for i in ids if ids.count(i) > 1}
            if duplicates:
                errors.append(f"duplicate {label} ids: {sorted(duplicates)}")
        return errors

    def _check_course_references(self) -> list[str]:
        errors = []
        subject_ids = {s.id for s in self.subjects}
        teacher_ids = {t.id for t in self.teachers}
        class_ids = {c.id for c in self.class_groups}
        room_types = {r.room_type for r in self.rooms}
        for course in self.courses:
            if course.subject_id not in subject_ids:
                errors.append(f"[{course.id}] unknown subject: {course.subject_id}")
                continue
            if course.class_group_id not in class_ids:
                errors.append(f"[{course.id}] unknown class: {course.class_group_id}")
            for teacher_id in course.teacher_ids:
                if teacher_id not in teacher_ids:
                    errors.append(f"[{course.id}] unknown teacher: {teacher_id}")
                    continue
                allowed = self.teacher_by_id[teacher_id].subject_ids
                if allowed and course.subject_id not in allowed:
                    errors.append(f"[{course.id}] {teacher_id} does not teach {course.subject_id}")
            room_type = self.subject_by_id[course.subject_id].required_room_type
            if room_type is not None and room_type not in room_types:
                errors.append(f"[{course.id}] no room of the required type: {room_type}")
        return errors

    def _check_parallel_groups(self) -> list[str]:
        errors = []
        for group, courses in self.courses_by_parallel_group.items():
            periods = {c.periods_per_week for c in courses}
            if len(periods) > 1:
                errors.append(f"parallel group {group} has differing weekly periods: {sorted(periods)}")
            blocks = {c.block_periods for c in courses}
            if len(blocks) > 1:
                errors.append(f"parallel group {group} has differing block periods: {sorted(blocks)}")
        return errors

    def _check_slot_validity(self) -> list[str]:
        errors = []
        sources: list[tuple[str, frozenset[TimeSlot]]] = [
            *((f"teacher {t.id} unavailable", t.unavailable) for t in self.teachers),
            *((f"class {c.id} unavailable", c.unavailable) for c in self.class_groups),
            *((f"course {c.id} fixed_slots", c.fixed_slots) for c in self.courses),
            *((f"course {c.id} forbidden_slots", c.forbidden_slots) for c in self.courses),
        ]
        for label, slots in sources:
            invalid = [str(s) for s in slots if not self.calendar.contains(s)]
            if invalid:
                errors.append(f"{label}: times not in the calendar {sorted(invalid)}")
        return errors
