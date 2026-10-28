"""Creating the CP-SAT decision variables.

The core variable is just `x[(course_id, slot)]`: "is course c placed at time s".
Teacher/class occupancy (`teacher_busy`, `class_busy`) are auxiliary booleans
derived from x, declared up front to make constraints like gaps and blocks easy
to write. The equalities tying them to x are posted by the hard constraints
(H3/H4), and those equalities are the no-clash constraints themselves.
"""

from collections import defaultdict
from typing import TYPE_CHECKING

from ortools.sat.python import cp_model

from teacher_scheduling.domain.entities import Course, TimeSlot
from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.domain.problem import TimetableProblem

if TYPE_CHECKING:
    from teacher_scheduling.solver.objective import DaySpan

DaySpans = dict[tuple[str, int], "DaySpan"]


class TimetableVariables:
    def __init__(self, model: cp_model.CpModel, problem: TimetableProblem) -> None:
        self.model = model
        self.problem = problem
        self.allowed_slots: dict[str, tuple[TimeSlot, ...]] = self._build_allowed_slots()
        self.x: dict[tuple[str, TimeSlot], cp_model.IntVar] = self._build_assignment_vars()

        self.teacher_lessons: dict[tuple[str, TimeSlot], list[cp_model.IntVar]] = self._collect_teacher_lessons()
        self.class_lessons: dict[tuple[str, TimeSlot], list[cp_model.IntVar]] = self._collect_class_lessons()
        self.teacher_busy = self._declare_busy_vars(self.teacher_lessons, "teacher_busy")
        self.class_busy = self._declare_busy_vars(self.class_lessons, "class_busy")

        # The per-day shape (DaySpan) auxiliaries are shared by several constraints, so they
        # are created once per model. TimetableVariables is new for every solve, so the cache lives exactly as long as the model.
        self._teacher_spans: DaySpans | None = None
        self._class_spans: DaySpans | None = None

    # ------------------------------------------------------------------ slot candidates

    def _course_allowed_slots(self, course: Course) -> set[TimeSlot]:
        """Removes hard-impossible times up front. Not creating the variables at all keeps the model light."""
        blocked = set(course.forbidden_slots)
        blocked |= set(self.problem.class_group_by_id[course.class_group_id].unavailable)
        for teacher_id in course.teacher_ids:
            blocked |= set(self.problem.teacher_by_id[teacher_id].unavailable)
        return {slot for slot in self.problem.calendar.slots if slot not in blocked}

    def _build_allowed_slots(self) -> dict[str, tuple[TimeSlot, ...]]:
        allowed = {course.id: self._course_allowed_slots(course) for course in self.problem.courses}

        # A parallel group always moves together, so its candidate slots are intersected too.
        for courses in self.problem.courses_by_parallel_group.values():
            shared = set.intersection(*(allowed[c.id] for c in courses))
            for course in courses:
                allowed[course.id] = shared

        return {
            course_id: tuple(sorted(slots, key=lambda s: (s.day, s.period))) for course_id, slots in allowed.items()
        }

    # ------------------------------------------------------------------ variables

    def _build_assignment_vars(self) -> dict[tuple[str, TimeSlot], cp_model.IntVar]:
        variables = {}
        for course in self.problem.courses:
            for slot in self.allowed_slots[course.id]:
                variables[course.id, slot] = self.model.new_bool_var(f"x[{course.id},{slot}]")
        return variables

    def _collect_teacher_lessons(self) -> dict[tuple[str, TimeSlot], list[cp_model.IntVar]]:
        terms: dict[tuple[str, TimeSlot], list[cp_model.IntVar]] = defaultdict(list)
        for course in self.problem.courses:
            for slot in self.allowed_slots[course.id]:
                for teacher_id in course.teacher_ids:
                    terms[teacher_id, slot].append(self.x[course.id, slot])
        return dict(terms)

    def _collect_class_lessons(self) -> dict[tuple[str, TimeSlot], list[cp_model.IntVar]]:
        """A parallel group must count only its representative course to share one cell."""
        terms: dict[tuple[str, TimeSlot], list[cp_model.IntVar]] = defaultdict(list)
        for course in self.occupancy_representatives():
            for slot in self.allowed_slots[course.id]:
                terms[course.class_group_id, slot].append(self.x[course.id, slot])
        return dict(terms)

    def _declare_busy_vars(
        self, lessons: dict[tuple[str, TimeSlot], list[cp_model.IntVar]], prefix: str
    ) -> dict[tuple[str, TimeSlot], cp_model.IntVar]:
        return {key: self.model.new_bool_var(f"{prefix}[{key[0]},{key[1]}]") for key in lessons}

    # ------------------------------------------------------------------ per-day shape (created lazily)

    def teacher_day_spans(self) -> DaySpans:
        """(teacher, weekday) -> DaySpan. The first constraint asking creates it; the rest share it."""
        if self._teacher_spans is None:
            self._teacher_spans = self._objective()._build_teacher_day_spans(self.model, self)
        return self._teacher_spans

    def class_day_spans(self) -> DaySpans:
        """(class, weekday) -> DaySpan."""
        if self._class_spans is None:
            self._class_spans = self._objective()._build_class_day_spans(self.model, self)
        return self._class_spans

    @staticmethod
    def _objective():
        """Imported lazily because objective imports this module (reverse dependency)."""
        from teacher_scheduling.solver import objective

        return objective

    # ------------------------------------------------------------------ lookup helpers

    def occupancy_representatives(self) -> tuple[Course, ...]:
        """Representative courses for class occupancy. A parallel group counts once."""
        seen: set[str] = set()
        representatives = []
        for course in self.problem.courses:
            if course.occupancy_key in seen:
                continue
            seen.add(course.occupancy_key)
            representatives.append(course)
        return tuple(representatives)

    def course_vars(self, course_id: str) -> list[cp_model.IntVar]:
        return [self.x[course_id, slot] for slot in self.allowed_slots[course_id]]

    def course_vars_on_day(self, course_id: str, day: Weekday) -> list[cp_model.IntVar]:
        return [self.x[course_id, slot] for slot in self.allowed_slots[course_id] if slot.day == day]

    def teacher_busy_on_day(self, teacher_id: str, day: Weekday) -> dict[int, cp_model.IntVar]:
        return self._busy_by_day(self.teacher_busy, teacher_id, day)

    def class_busy_on_day(self, class_id: str, day: Weekday) -> dict[int, cp_model.IntVar]:
        return self._busy_by_day(self.class_busy, class_id, day)

    @staticmethod
    def _busy_by_day(
        busy: dict[tuple[str, TimeSlot], cp_model.IntVar], owner_id: str, day: Weekday
    ) -> dict[int, cp_model.IntVar]:
        return {
            slot.period: var
            for (busy_owner_id, slot), var in busy.items()
            if busy_owner_id == owner_id and slot.day == day
        }
