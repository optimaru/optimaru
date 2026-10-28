"""Timetable validation.

Independently of the solver, checks again that a timetable respects the hard
constraints H1–H10 and recomputes the soft penalties by hand. Used by the tests
and the CLI.

Validation against the default registry (hard H1–H10, soft S1–S6). Custom
hard_constraints / soft_terms injected into `solve()` are not checked here —
validating custom constraints is up to the caller.
"""

from collections import Counter, defaultdict

from teacher_scheduling.domain.entities import TimeSlot
from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.domain.problem import TimetableProblem
from teacher_scheduling.domain.solution import ScheduledLesson, Timetable


def validate_timetable(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    """Returns the hard-violation messages. Empty means a valid timetable."""
    checks = (
        _check_slot_validity,
        _check_weekly_counts,  # H1
        _check_fixed_slots,  # H2
        _check_class_conflicts,  # H3
        _check_teacher_conflicts,  # H4
        _check_parallel_groups,  # H5
        _check_rooms,  # H6 + placement consistency
        _check_daily_course_limits,  # H7
        _check_blocks,  # H8
        _check_teacher_workload,  # H9
        _check_teacher_consecutive,  # H10
    )
    violations: list[str] = []
    for check in checks:
        violations.extend(check(problem, timetable))
    return violations


def compute_penalties(problem: TimetableProblem, timetable: Timetable) -> dict[str, int]:
    """Recounts the soft penalties (S1–S6) directly from the timetable. For checking what the solver reports."""
    teacher_periods = _busy_periods(timetable, by_teacher=True)
    class_periods = _busy_periods(timetable, by_teacher=False)

    avoided = non_preferred = 0
    for lesson in timetable.lessons:
        subject = problem.subject_by_id[lesson.subject_id]
        if lesson.slot.period in subject.avoided_periods:
            avoided += 1
        if subject.preferred_periods and lesson.slot.period not in subject.preferred_periods:
            non_preferred += 1

    return {
        "teacher_gap": sum(_gap_count(periods) for periods in teacher_periods.values()),
        "teacher_workday": len(teacher_periods),  # keys exist only for (teacher, weekday) pairs with lessons
        "teacher_free_day": sum(
            1 for (teacher_id, day) in teacher_periods if day in problem.teacher_by_id[teacher_id].preferred_free_days
        ),
        "class_gap": sum(_gap_count(periods) for periods in class_periods.values()),
        "avoided_period": avoided,
        "non_preferred_period": non_preferred,
    }


# ---------------------------------------------------------------- shared aggregation


def _busy_periods(timetable: Timetable, *, by_teacher: bool) -> dict[tuple[str, Weekday], set[int]]:
    """(teacher|class, weekday) -> set of periods with lessons."""
    busy: dict[tuple[str, Weekday], set[int]] = defaultdict(set)
    for lesson in timetable.lessons:
        owners = lesson.teacher_ids if by_teacher else (lesson.class_group_id,)
        for owner in owners:
            busy[owner, lesson.slot.day].add(lesson.slot.period)
    return busy


def _gap_count(periods: set[int]) -> int:
    return max(periods) - min(periods) + 1 - len(periods)


def _max_consecutive_run(periods: set[int]) -> int:
    longest = current = 0
    previous: int | None = None
    for period in sorted(periods):
        current = current + 1 if previous == period - 1 else 1
        longest = max(longest, current)
        previous = period
    return longest


def _lessons_of_course(timetable: Timetable, course_id: str) -> list[ScheduledLesson]:
    return [lesson for lesson in timetable.lessons if lesson.course_id == course_id]


# ---------------------------------------------------------------- individual checks


def _check_slot_validity(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    """Lesson cells lie in the calendar and do not overlap forbidden/unavailable times."""
    violations = []
    for lesson in timetable.lessons:
        course = problem.course_by_id[lesson.course_id]
        slot = lesson.slot
        if not problem.calendar.contains(slot):
            violations.append(f"[{course.id}] time {slot} not in the calendar")
        if slot in course.forbidden_slots:
            violations.append(f"[{course.id}] placed in forbidden time {slot}")
        if slot in problem.class_group_by_id[course.class_group_id].unavailable:
            violations.append(f"[{course.id}] placed in class-unavailable time {slot}")
        for teacher_id in lesson.teacher_ids:
            if slot in problem.teacher_by_id[teacher_id].unavailable:
                violations.append(f"[{course.id}] placed in time {slot} unavailable to teacher {teacher_id}")
    return violations


def _check_weekly_counts(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    for course in problem.courses:
        count = len(_lessons_of_course(timetable, course.id))
        if count != course.periods_per_week:
            violations.append(f"[{course.id}] {count} periods placed for {course.periods_per_week} weekly periods")
    return violations


def _check_fixed_slots(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    for course in problem.courses:
        scheduled = {lesson.slot for lesson in _lessons_of_course(timetable, course.id)}
        for slot in course.fixed_slots:
            if slot not in scheduled:
                violations.append(f"[{course.id}] fixed time {slot} is empty")
    return violations


def _check_class_conflicts(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    """One occupancy unit per class and time (a parallel group counts as one)."""
    violations = []
    occupied: dict[tuple[str, TimeSlot], set[str]] = defaultdict(set)
    for lesson in timetable.lessons:
        course = problem.course_by_id[lesson.course_id]
        occupied[lesson.class_group_id, lesson.slot].add(course.occupancy_key)
    for (class_id, slot), keys in occupied.items():
        if len(keys) > 1:
            violations.append(f"class {class_id} has lessons {sorted(keys)} at the same time {slot}")
    return violations


def _check_teacher_conflicts(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    load: Counter[tuple[str, TimeSlot]] = Counter()
    for lesson in timetable.lessons:
        for teacher_id in lesson.teacher_ids:
            load[teacher_id, lesson.slot] += 1
    for (teacher_id, slot), count in load.items():
        if count > 1:
            violations.append(f"teacher {teacher_id} has {count} lessons at {slot}")
    return violations


def _check_parallel_groups(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    for group, courses in problem.courses_by_parallel_group.items():
        slot_sets = {
            course.id: {lesson.slot for lesson in _lessons_of_course(timetable, course.id)} for course in courses
        }
        reference = slot_sets[courses[0].id]
        for course_id, slots in slot_sets.items():
            if slots != reference:
                violations.append(f"{course_id} of parallel group {group} runs at a different time")
    return violations


def _check_rooms(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    room_by_id = {room.id: room for room in problem.rooms}
    usage: Counter[tuple[str, TimeSlot]] = Counter()
    demand: Counter[tuple[str, TimeSlot]] = Counter()
    for lesson in timetable.lessons:
        required = problem.required_room_type(problem.course_by_id[lesson.course_id])
        if required is None:
            if lesson.room_id is not None:
                violations.append(f"[{lesson.course_id}] needs no special room but got {lesson.room_id}")
            continue
        demand[required, lesson.slot] += 1
        if lesson.room_id is None:
            violations.append(f"[{lesson.course_id}] no {required} assigned at {lesson.slot}")
            continue
        room = room_by_id.get(lesson.room_id)
        if room is None or room.room_type != required:
            violations.append(f"[{lesson.course_id}] wrong special room {lesson.room_id} at {lesson.slot}")
            continue
        usage[lesson.room_id, lesson.slot] += 1
    for (room_id, slot), count in usage.items():
        if count > 1:
            violations.append(f"special room {room_id} used {count} times at {slot}")
    for (room_type, slot), count in demand.items():
        capacity = len(problem.rooms_by_type.get(room_type, ()))
        if count > capacity:
            violations.append(f"{room_type} demand {count} > {capacity} rooms at {slot}")
    return violations


def _check_daily_course_limits(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    for course in problem.courses:
        per_day: Counter[Weekday] = Counter(lesson.slot.day for lesson in _lessons_of_course(timetable, course.id))
        limit = course.effective_max_periods_per_day
        for day, count in per_day.items():
            if count > limit:
                violations.append(f"[{course.id}] {count} periods on {day.label} (daily cap {limit})")
    return violations


def _check_blocks(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    """A block course needs block_periods or more consecutive periods on some day."""
    violations = []
    for course in problem.courses:
        if not course.has_block:
            continue
        by_day: dict[Weekday, set[int]] = defaultdict(set)
        for lesson in _lessons_of_course(timetable, course.id):
            by_day[lesson.slot.day].add(lesson.slot.period)
        if not any(_max_consecutive_run(periods) >= course.block_periods for periods in by_day.values()):
            violations.append(f"[{course.id}] no block of {course.block_periods} consecutive periods")
    return violations


def _check_teacher_workload(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    busy = _busy_periods(timetable, by_teacher=True)
    for teacher in problem.teachers:
        days = {day: periods for (teacher_id, day), periods in busy.items() if teacher_id == teacher.id}
        total = sum(len(periods) for periods in days.values())
        if teacher.max_periods_per_week is not None and total > teacher.max_periods_per_week:
            violations.append(f"teacher {teacher.id} has {total} periods a week (cap {teacher.max_periods_per_week})")
        for day, periods in days.items():
            if len(periods) > teacher.max_periods_per_day:
                violations.append(
                    f"teacher {teacher.id} has {len(periods)} periods on {day.label} (cap {teacher.max_periods_per_day})"
                )
    return violations


def _check_teacher_consecutive(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    for (teacher_id, day), periods in _busy_periods(timetable, by_teacher=True).items():
        limit = problem.teacher_by_id[teacher_id].max_consecutive_periods
        run = _max_consecutive_run(periods)
        if run > limit:
            violations.append(f"teacher {teacher_id} has {run} consecutive periods on {day.label} (cap {limit})")
    return violations


__all__ = ["compute_penalties", "validate_timetable"]
