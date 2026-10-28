"""편성 결과를 터미널용 텍스트 리포트로 렌더링.

학급별/교사별 시간표를 요일(열) x 교시(행) 그리드로 그리고,
벌점 요약을 붙인다. 한글은 터미널에서 두 칸을 차지하므로 표시 폭 기준으로 정렬한다.
"""

import unicodedata

from teacher_scheduling.domain.entities import TimeSlot
from teacher_scheduling.domain.problem import SchoolCalendar, TimetableProblem
from teacher_scheduling.domain.solution import ScheduledLesson, Timetable
from teacher_scheduling.solver.config import ObjectiveWeights

_ROW_LABEL_WIDTH = 6
_MIN_CELL_WIDTH = 4


def render_report(problem: TimetableProblem, timetable: Timetable, weights: ObjectiveWeights | None = None) -> str:
    """요약 + 학급별 그리드 + 교사별 그리드를 하나의 문자열로 묶는다."""
    sections = [render_summary(timetable, weights)]
    if timetable.is_solved:
        sections.append(render_class_grids(problem, timetable))
        sections.append(render_teacher_grids(problem, timetable))
    return "\n\n".join(sections)


def render_summary(timetable: Timetable, weights: ObjectiveWeights | None = None) -> str:
    lines = [f"상태: {timetable.status}  (탐색 {timetable.wall_time_seconds:.2f}초)"]
    if timetable.objective_value is not None:
        lines.append(f"목적함수: {timetable.objective_value:.0f}  (하한 {timetable.best_objective_bound:.0f})")
    if timetable.penalties:
        lines.append("벌점(가중치 적용 전 횟수):")
        for name, count in timetable.penalties.items():
            weight = weights.weight_for(name) if weights is not None else None
            suffix = f" x {weight} = {count * weight}" if weight is not None else ""
            lines.append(f"  - {name}: {count}{suffix}")
    for note in timetable.notes:
        lines.append(f"참고: {note}")
    return "\n".join(lines)


def render_class_grids(problem: TimetableProblem, timetable: Timetable) -> str:
    grids = []
    by_class = timetable.by_class_group()
    for class_group in problem.class_groups:
        cells = {slot: _class_cell(problem, lessons) for slot, lessons in by_class.get(class_group.id, {}).items()}
        grids.append(_render_grid(problem.calendar, f"{class_group.name} 시간표", cells))
    return "\n\n".join(grids)


def render_teacher_grids(problem: TimetableProblem, timetable: Timetable) -> str:
    grids = []
    by_teacher = timetable.by_teacher()
    for teacher in problem.teachers:
        if teacher.id not in by_teacher:
            continue
        cells = {slot: _teacher_cell(problem, lessons) for slot, lessons in by_teacher[teacher.id].items()}
        grids.append(_render_grid(problem.calendar, f"{teacher.name} 시간표", cells))
    return "\n\n".join(grids)


# ---------------------------------------------------------------- 셀 내용


def _class_cell(problem: TimetableProblem, lessons: list[ScheduledLesson]) -> str:
    """학급 뷰 한 칸: 과목(교사)@특별실. 분반이면 교사를 / 로 나열한다."""
    lessons = sorted(lessons, key=lambda lesson: lesson.course_id)
    subjects = _unique([problem.subject_by_id[lesson.subject_id].name for lesson in lessons])
    teachers = _unique(
        [problem.teacher_by_id[teacher_id].name for lesson in lessons for teacher_id in lesson.teacher_ids]
    )
    rooms = _unique([_room_name(problem, lesson.room_id) for lesson in lessons if lesson.room_id])
    text = f"{'/'.join(subjects)}({'/'.join(teachers)})"
    return text + (f"@{'/'.join(rooms)}" if rooms else "")


def _teacher_cell(problem: TimetableProblem, lessons: list[ScheduledLesson]) -> str:
    """교사 뷰 한 칸: 학급 과목@특별실."""
    parts = []
    for lesson in sorted(lessons, key=lambda lesson: lesson.course_id):
        class_name = problem.class_group_by_id[lesson.class_group_id].name
        subject_name = problem.subject_by_id[lesson.subject_id].name
        text = f"{class_name} {subject_name}"
        if lesson.room_id:
            text += f"@{_room_name(problem, lesson.room_id)}"
        parts.append(text)
    return "/".join(parts)


def _room_name(problem: TimetableProblem, room_id: str) -> str:
    for room in problem.rooms:
        if room.id == room_id:
            return room.name
    return room_id


def _unique(items: list[str]) -> list[str]:
    return list(dict.fromkeys(items))


# ---------------------------------------------------------------- 그리드


def _render_grid(calendar: SchoolCalendar, title: str, cells: dict[TimeSlot, str]) -> str:
    width = max([_MIN_CELL_WIDTH, *(_display_width(text) for text in cells.values())])
    lines = [f"[{title}]"]
    header = _pad("", _ROW_LABEL_WIDTH) + "".join(f" | {_pad(day.label, width)}" for day in calendar.days)
    lines.append(header)
    lines.append("-" * _display_width(header))
    for period in range(1, calendar.max_periods + 1):
        row = _pad(f"{period}교시", _ROW_LABEL_WIDTH)
        for day in calendar.days:
            slot = TimeSlot(day=day, period=period)
            text = cells.get(slot, "") if calendar.contains(slot) else "-"
            row += f" | {_pad(text, width)}"
        lines.append(row)
    return "\n".join(lines)


def _display_width(text: str) -> int:
    """터미널 표시 폭. 한글 등 전각 문자는 2칸으로 센다."""
    return sum(2 if unicodedata.east_asian_width(char) in "WF" else 1 for char in text)


def _pad(text: str, width: int) -> str:
    return text + " " * max(0, width - _display_width(text))


__all__ = ["render_class_grids", "render_report", "render_summary", "render_teacher_grids"]
