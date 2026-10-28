"""Renders a timetabling result as a text report for the terminal.

Draws per-class and per-teacher timetables as weekday (columns) x period (rows)
grids and appends a penalty summary. Wide characters take two terminal cells,
so alignment uses display width.
"""

import unicodedata

from teacher_scheduling.domain.entities import TimeSlot
from teacher_scheduling.domain.problem import SchoolCalendar, TimetableProblem
from teacher_scheduling.domain.solution import ScheduledLesson, Timetable
from teacher_scheduling.solver.config import ObjectiveWeights

_ROW_LABEL_WIDTH = 6
_MIN_CELL_WIDTH = 4


def render_report(problem: TimetableProblem, timetable: Timetable, weights: ObjectiveWeights | None = None) -> str:
    """Bundles the summary + per-class grids + per-teacher grids into one string."""
    sections = [render_summary(timetable, weights)]
    if timetable.is_solved:
        sections.append(render_class_grids(problem, timetable))
        sections.append(render_teacher_grids(problem, timetable))
    return "\n\n".join(sections)


def render_summary(timetable: Timetable, weights: ObjectiveWeights | None = None) -> str:
    lines = [f"status: {timetable.status}  (search {timetable.wall_time_seconds:.2f} s)"]
    if timetable.objective_value is not None:
        lines.append(f"objective: {timetable.objective_value:.0f}  (bound {timetable.best_objective_bound:.0f})")
    if timetable.penalties:
        lines.append("penalties (counts before weighting):")
        for name, count in timetable.penalties.items():
            weight = weights.weight_for(name) if weights is not None else None
            suffix = f" x {weight} = {count * weight}" if weight is not None else ""
            lines.append(f"  - {name}: {count}{suffix}")
    for note in timetable.notes:
        lines.append(f"note: {note}")
    return "\n".join(lines)


def render_class_grids(problem: TimetableProblem, timetable: Timetable) -> str:
    grids = []
    by_class = timetable.by_class_group()
    for class_group in problem.class_groups:
        cells = {slot: _class_cell(problem, lessons) for slot, lessons in by_class.get(class_group.id, {}).items()}
        grids.append(_render_grid(problem.calendar, f"{class_group.name} timetable", cells))
    return "\n\n".join(grids)


def render_teacher_grids(problem: TimetableProblem, timetable: Timetable) -> str:
    grids = []
    by_teacher = timetable.by_teacher()
    for teacher in problem.teachers:
        if teacher.id not in by_teacher:
            continue
        cells = {slot: _teacher_cell(problem, lessons) for slot, lessons in by_teacher[teacher.id].items()}
        grids.append(_render_grid(problem.calendar, f"{teacher.name} timetable", cells))
    return "\n\n".join(grids)


# ---------------------------------------------------------------- cell contents


def _class_cell(problem: TimetableProblem, lessons: list[ScheduledLesson]) -> str:
    """One cell of the class view: subject(teacher)@room. Parallel sections list teachers with /."""
    lessons = sorted(lessons, key=lambda lesson: lesson.course_id)
    subjects = _unique([problem.subject_by_id[lesson.subject_id].name for lesson in lessons])
    teachers = _unique(
        [problem.teacher_by_id[teacher_id].name for lesson in lessons for teacher_id in lesson.teacher_ids]
    )
    rooms = _unique([_room_name(problem, lesson.room_id) for lesson in lessons if lesson.room_id])
    text = f"{'/'.join(subjects)}({'/'.join(teachers)})"
    return text + (f"@{'/'.join(rooms)}" if rooms else "")


def _teacher_cell(problem: TimetableProblem, lessons: list[ScheduledLesson]) -> str:
    """One cell of the teacher view: class subject@room."""
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


# ---------------------------------------------------------------- grid


def _render_grid(calendar: SchoolCalendar, title: str, cells: dict[TimeSlot, str]) -> str:
    width = max([_MIN_CELL_WIDTH, *(_display_width(text) for text in cells.values())])
    lines = [f"[{title}]"]
    header = _pad("", _ROW_LABEL_WIDTH) + "".join(f" | {_pad(day.label, width)}" for day in calendar.days)
    lines.append(header)
    lines.append("-" * _display_width(header))
    for period in range(1, calendar.max_periods + 1):
        row = _pad(f"P{period}", _ROW_LABEL_WIDTH)
        for day in calendar.days:
            slot = TimeSlot(day=day, period=period)
            text = cells.get(slot, "") if calendar.contains(slot) else "-"
            row += f" | {_pad(text, width)}"
        lines.append(row)
    return "\n".join(lines)


def _display_width(text: str) -> int:
    """Terminal display width. Wide (East Asian) characters count as 2 cells."""
    return sum(2 if unicodedata.east_asian_width(char) in "WF" else 1 for char in text)


def _pad(text: str, width: int) -> str:
    return text + " " * max(0, width - _display_width(text))


__all__ = ["render_class_grids", "render_report", "render_summary", "render_teacher_grids"]
