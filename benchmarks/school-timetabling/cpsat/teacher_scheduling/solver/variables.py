"""CP-SAT 결정변수 생성.

핵심 변수는 `x[(course_id, slot)]` 하나뿐이다. "강좌 c 를 시간 s 에 배치하는가".
교사/학급 점유 여부(`teacher_busy`, `class_busy`)는 x 로부터 파생되는 보조 불리언으로,
공강·연강 같은 제약을 쓰기 쉽게 만들기 위해 미리 선언해 둔다.
x 와 묶는 등식은 하드 제약(H3/H4)에서 걸며, 그 등식이 곧 충돌 금지 제약이 된다.
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

        # 하루 모양(DaySpan) 보조 변수는 여러 제약이 나눠 쓰므로 모델당 한 번만
        # 만든다. TimetableVariables 는 solve 마다 새로 생기니 캐시 수명이 곧 모델 수명이다.
        self._teacher_spans: DaySpans | None = None
        self._class_spans: DaySpans | None = None

    # ------------------------------------------------------------------ 슬롯 후보

    def _course_allowed_slots(self, course: Course) -> set[TimeSlot]:
        """하드하게 불가능한 시간을 미리 걷어낸다. 변수 자체를 안 만들어야 모델이 가벼워진다."""
        blocked = set(course.forbidden_slots)
        blocked |= set(self.problem.class_group_by_id[course.class_group_id].unavailable)
        for teacher_id in course.teacher_ids:
            blocked |= set(self.problem.teacher_by_id[teacher_id].unavailable)
        return {slot for slot in self.problem.calendar.slots if slot not in blocked}

    def _build_allowed_slots(self) -> dict[str, tuple[TimeSlot, ...]]:
        allowed = {course.id: self._course_allowed_slots(course) for course in self.problem.courses}

        # 분반(parallel group)은 항상 같은 시간에 움직이므로 후보 슬롯도 교집합으로 맞춘다.
        for courses in self.problem.courses_by_parallel_group.values():
            shared = set.intersection(*(allowed[c.id] for c in courses))
            for course in courses:
                allowed[course.id] = shared

        return {
            course_id: tuple(sorted(slots, key=lambda s: (s.day, s.period))) for course_id, slots in allowed.items()
        }

    # ------------------------------------------------------------------ 변수

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
        """분반 그룹은 대표 강좌 하나만 세야 한 칸을 나눠 쓰는 것으로 계산된다."""
        terms: dict[tuple[str, TimeSlot], list[cp_model.IntVar]] = defaultdict(list)
        for course in self.occupancy_representatives():
            for slot in self.allowed_slots[course.id]:
                terms[course.class_group_id, slot].append(self.x[course.id, slot])
        return dict(terms)

    def _declare_busy_vars(
        self, lessons: dict[tuple[str, TimeSlot], list[cp_model.IntVar]], prefix: str
    ) -> dict[tuple[str, TimeSlot], cp_model.IntVar]:
        return {key: self.model.new_bool_var(f"{prefix}[{key[0]},{key[1]}]") for key in lessons}

    # ------------------------------------------------------------------ 하루 모양 (지연 생성)

    def teacher_day_spans(self) -> DaySpans:
        """(교사, 요일) -> DaySpan. 처음 물어본 제약이 만들고 나머지는 그걸 나눠 쓴다."""
        if self._teacher_spans is None:
            self._teacher_spans = self._objective()._build_teacher_day_spans(self.model, self)
        return self._teacher_spans

    def class_day_spans(self) -> DaySpans:
        """(학급, 요일) -> DaySpan."""
        if self._class_spans is None:
            self._class_spans = self._objective()._build_class_day_spans(self.model, self)
        return self._class_spans

    @staticmethod
    def _objective():
        """objective 가 이 모듈을 임포트하므로(역방향 의존) 지연 임포트한다."""
        from teacher_scheduling.solver import objective

        return objective

    # ------------------------------------------------------------------ 조회 헬퍼

    def occupancy_representatives(self) -> tuple[Course, ...]:
        """학급 점유 계산에 쓸 대표 강좌. 분반 그룹은 그룹당 하나만 센다."""
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
