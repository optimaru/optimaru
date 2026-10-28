"""편성 결과 검증.

솔버와 독립적으로, 결과 시간표가 하드 제약 H1~H10 을 지키는지 다시 확인하고
소프트 벌점을 손으로 다시 계산한다. 테스트와 CLI 가 함께 쓴다.

기본 레지스트리(하드 H1~H10, 소프트 S1~S6) 기준의 검증이다. `solve()` 에
커스텀 hard_constraints / soft_terms 를 주입했더라도 그 커스텀 규칙은 여기서
검사하지 않는다 — 커스텀 제약의 검증은 호출자 몫이다.
"""

from collections import Counter, defaultdict

from teacher_scheduling.domain.entities import TimeSlot
from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.domain.problem import TimetableProblem
from teacher_scheduling.domain.solution import ScheduledLesson, Timetable


def validate_timetable(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    """하드 제약 위반 메시지 목록을 돌려준다. 비어 있으면 유효한 시간표다."""
    checks = (
        _check_slot_validity,
        _check_weekly_counts,  # H1
        _check_fixed_slots,  # H2
        _check_class_conflicts,  # H3
        _check_teacher_conflicts,  # H4
        _check_parallel_groups,  # H5
        _check_rooms,  # H6 + 배정 일관성
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
    """소프트 벌점(S1~S6)을 결과 시간표에서 직접 다시 센다. 솔버 보고값 검증용."""
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
        "teacher_workday": len(teacher_periods),  # 키는 수업이 있는 (교사, 요일)만 생긴다
        "teacher_free_day": sum(
            1 for (teacher_id, day) in teacher_periods if day in problem.teacher_by_id[teacher_id].preferred_free_days
        ),
        "class_gap": sum(_gap_count(periods) for periods in class_periods.values()),
        "avoided_period": avoided,
        "non_preferred_period": non_preferred,
    }


# ---------------------------------------------------------------- 공용 집계


def _busy_periods(timetable: Timetable, *, by_teacher: bool) -> dict[tuple[str, Weekday], set[int]]:
    """(교사|학급, 요일) -> 수업이 있는 교시 집합."""
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


# ---------------------------------------------------------------- 개별 검사


def _check_slot_validity(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    """수업 칸이 캘린더 안에 있고, 금지/불가 시간과 겹치지 않는지."""
    violations = []
    for lesson in timetable.lessons:
        course = problem.course_by_id[lesson.course_id]
        slot = lesson.slot
        if not problem.calendar.contains(slot):
            violations.append(f"[{course.id}] 캘린더에 없는 시간 {slot}")
        if slot in course.forbidden_slots:
            violations.append(f"[{course.id}] 금지 시간 {slot} 에 배정")
        if slot in problem.class_group_by_id[course.class_group_id].unavailable:
            violations.append(f"[{course.id}] 학급 불가 시간 {slot} 에 배정")
        for teacher_id in lesson.teacher_ids:
            if slot in problem.teacher_by_id[teacher_id].unavailable:
                violations.append(f"[{course.id}] 교사 {teacher_id} 불가 시간 {slot} 에 배정")
    return violations


def _check_weekly_counts(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    for course in problem.courses:
        count = len(_lessons_of_course(timetable, course.id))
        if count != course.periods_per_week:
            violations.append(f"[{course.id}] 주당 시수 {course.periods_per_week} 인데 {count}시간 배정")
    return violations


def _check_fixed_slots(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    for course in problem.courses:
        scheduled = {lesson.slot for lesson in _lessons_of_course(timetable, course.id)}
        for slot in course.fixed_slots:
            if slot not in scheduled:
                violations.append(f"[{course.id}] 고정 시간 {slot} 이 비어 있음")
    return violations


def _check_class_conflicts(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    """같은 학급, 같은 시간에는 하나의 점유 단위(분반 그룹은 통째로 하나)만 허용."""
    violations = []
    occupied: dict[tuple[str, TimeSlot], set[str]] = defaultdict(set)
    for lesson in timetable.lessons:
        course = problem.course_by_id[lesson.course_id]
        occupied[lesson.class_group_id, lesson.slot].add(course.occupancy_key)
    for (class_id, slot), keys in occupied.items():
        if len(keys) > 1:
            violations.append(f"학급 {class_id} 이 {slot} 에 수업 {sorted(keys)} 중복")
    return violations


def _check_teacher_conflicts(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    load: Counter[tuple[str, TimeSlot]] = Counter()
    for lesson in timetable.lessons:
        for teacher_id in lesson.teacher_ids:
            load[teacher_id, lesson.slot] += 1
    for (teacher_id, slot), count in load.items():
        if count > 1:
            violations.append(f"교사 {teacher_id} 가 {slot} 에 수업 {count}개")
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
                violations.append(f"분반 그룹 {group} 의 {course_id} 가 다른 시간에 진행")
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
                violations.append(f"[{lesson.course_id}] 특별실이 필요 없는데 {lesson.room_id} 배정")
            continue
        demand[required, lesson.slot] += 1
        if lesson.room_id is None:
            violations.append(f"[{lesson.course_id}] {lesson.slot} 에 {required} 미배정")
            continue
        room = room_by_id.get(lesson.room_id)
        if room is None or room.room_type != required:
            violations.append(f"[{lesson.course_id}] {lesson.slot} 에 잘못된 특별실 {lesson.room_id}")
            continue
        usage[lesson.room_id, lesson.slot] += 1
    for (room_id, slot), count in usage.items():
        if count > 1:
            violations.append(f"특별실 {room_id} 이 {slot} 에 {count}번 사용")
    for (room_type, slot), count in demand.items():
        capacity = len(problem.rooms_by_type.get(room_type, ()))
        if count > capacity:
            violations.append(f"{slot} 의 {room_type} 수요 {count} > 보유 {capacity}")
    return violations


def _check_daily_course_limits(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    for course in problem.courses:
        per_day: Counter[Weekday] = Counter(lesson.slot.day for lesson in _lessons_of_course(timetable, course.id))
        limit = course.effective_max_periods_per_day
        for day, count in per_day.items():
            if count > limit:
                violations.append(f"[{course.id}] {day.label}요일에 {count}시간 (일일 상한 {limit})")
    return violations


def _check_blocks(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    """블록타임 강좌는 어느 하루에 block_periods 이상의 연속 배치가 있어야 한다."""
    violations = []
    for course in problem.courses:
        if not course.has_block:
            continue
        by_day: dict[Weekday, set[int]] = defaultdict(set)
        for lesson in _lessons_of_course(timetable, course.id):
            by_day[lesson.slot.day].add(lesson.slot.period)
        if not any(_max_consecutive_run(periods) >= course.block_periods for periods in by_day.values()):
            violations.append(f"[{course.id}] {course.block_periods}연강이 없음")
    return violations


def _check_teacher_workload(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    busy = _busy_periods(timetable, by_teacher=True)
    for teacher in problem.teachers:
        days = {day: periods for (teacher_id, day), periods in busy.items() if teacher_id == teacher.id}
        total = sum(len(periods) for periods in days.values())
        if teacher.max_periods_per_week is not None and total > teacher.max_periods_per_week:
            violations.append(f"교사 {teacher.id} 주당 {total}시간 (상한 {teacher.max_periods_per_week})")
        for day, periods in days.items():
            if len(periods) > teacher.max_periods_per_day:
                violations.append(
                    f"교사 {teacher.id} {day.label}요일 {len(periods)}시간 (상한 {teacher.max_periods_per_day})"
                )
    return violations


def _check_teacher_consecutive(problem: TimetableProblem, timetable: Timetable) -> list[str]:
    violations = []
    for (teacher_id, day), periods in _busy_periods(timetable, by_teacher=True).items():
        limit = problem.teacher_by_id[teacher_id].max_consecutive_periods
        run = _max_consecutive_run(periods)
        if run > limit:
            violations.append(f"교사 {teacher_id} {day.label}요일 연속 {run}시간 (상한 {limit})")
    return violations


__all__ = ["compute_penalties", "validate_timetable"]
