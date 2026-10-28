"""Exporting a timetable — the NEIS bulk file (CSV) and printable Excel.

Matches Comcigan's "print" menu.

- `neis_bundle()`   : the NEIS bulk file. One CSV per class.
- `timetable_workbook()` : Excel with the overall / class / teacher / special-room timetables.

NEIS (the Korean national school information system) documents a per-class file
name rule, `기초시간표(<school year>-<semester>,<grade>-<class>).csv`, and a
weekday x period grid. The text in a cell (subject name / teacher name) must
equal the names registered in NEIS's course list for the target field to fill
in, so `official_subject_names` allows official subject names instead of short
ones. The NEIS-facing labels below stay Korean because NEIS reads them.

The CSV's finer structure (header or not, number of weekday columns, subject
and teacher in one cell or not) is not in the manual, so the defaults are
reverse-engineered from screenshots — if they disagree with the real NEIS,
change `NeisFormat`, not the code. See the `NeisFormat` docstring.
"""

from __future__ import annotations

import csv
import io
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Any

from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.domain.problem import TimetableProblem
from teacher_scheduling.domain.solution import ScheduledLesson, Timetable

# NEIS weekday headers: Monday … Sunday.
DAY_LABELS = ("월요일", "화요일", "수요일", "목요일", "금요일", "토요일", "일요일")


@dataclass(frozen=True)
class NeisFormat:
    """Finer format of the NEIS base-timetable CSV.

    Chapter 10 of the manual only shows how to **create and upload** the file —
    confirmed are the file name rule
    (`기초시간표(<school year>-<semester>,<grade>-<class>).csv`), the weekday x
    period grid, and that both subject and teacher names are matched; the CSV's
    inner structure (header or not, column count, subject and teacher in one
    cell or not) is not described.

    The defaults are therefore reverse-engineered from the manual's screenshots,
    and left open so they can be adjusted after checking against a real NEIS
    import **without changing code**. To check: in NEIS, upload the file via
    school curriculum -> timetable management -> base timetable -> search ->
    import -> from file, and see whether the "source" row fills in. If it does,
    the format is right; if only "target" stays empty, it is a name-matching
    issue (manual 10-B).
    """

    #: Number of weekday columns. Default 5 (Mon–Fri). The NEIS screen has 7
    #: columns (Mon–Sun), so 7 works too — extra weekdays without lessons are empty columns.
    day_count: int = 5

    #: Whether to write the `교시,월요일,...` (period, Monday, ...) header row.
    include_header: bool = True

    #: First-column notation. `"교시"` (period) gives `1교시`, `""` gives `1`.
    period_suffix: str = "교시"

    #: True gives each weekday a subject column and a teacher column.
    #: False writes `subject{teacher_separator}teacher` in one cell.
    teacher_column: bool = False
    teacher_separator: str = "/"

    #: Header suffix of the separate teacher columns (`월요일 교사` = Monday teacher).
    teacher_header_suffix: str = " 교사"

    #: What to do when parallel sections put several courses in one cell.
    #: With a separator all are written (e.g. `Tech/Kim;HomeEc/Lee`); with `None`
    #: only the first course is kept — use `None` if NEIS reads only one subject per cell.
    parallel_separator: str | None = ";"

    delimiter: str = ","

    #: NEIS takes Korean CSVs in the EUC-KR family. If a character cannot be
    #: encoded that way, fall back to `utf-8-sig`.
    encoding: str = "cp949"

    def __post_init__(self) -> None:
        if not 1 <= self.day_count <= len(DAY_LABELS):
            raise ValueError(f"day_count must be within 1–{len(DAY_LABELS)}: {self.day_count}")

    def period_label(self, period: int) -> str:
        return f"{period}{self.period_suffix}"


@dataclass(frozen=True)
class ExportOptions:
    school_year: int = 0
    semester: int = 1
    #: short subject name -> NEIS official subject name. Without one, the short name is used.
    official_subject_names: dict[str, str] | None = None
    #: The NEIS CSV's finer format. Change only this if it disagrees with the real NEIS.
    neis: NeisFormat = field(default_factory=NeisFormat)

    def subject_label(self, subject_id: str, fallback: str) -> str:
        if self.official_subject_names:
            return self.official_subject_names.get(subject_id, fallback)
        return fallback


# ---------------------------------------------------------------- building the grid


def _grid(
    problem: TimetableProblem,
    lessons: list[ScheduledLesson],
) -> dict[tuple[Weekday, int], list[ScheduledLesson]]:
    grid: dict[tuple[Weekday, int], list[ScheduledLesson]] = defaultdict(list)
    for lesson in lessons:
        grid[lesson.slot.day, lesson.slot.period].append(lesson)
    return grid


def _class_lessons(timetable: Timetable, class_id: str) -> list[ScheduledLesson]:
    return [lesson for lesson in timetable.lessons if lesson.class_group_id == class_id]


def _teacher_lessons(timetable: Timetable, teacher_id: str) -> list[ScheduledLesson]:
    return [lesson for lesson in timetable.lessons if teacher_id in lesson.teacher_ids]


# ---------------------------------------------------------------- NEIS


def neis_bundle(
    problem: TimetableProblem,
    timetable: Timetable,
    options: ExportOptions | None = None,
) -> dict[str, bytes]:
    """The per-class NEIS base-timetable CSVs. Returns `{file name: content}`.

    The default writes `subject/teacher` in one cell. NEIS matches both the
    subject and the teacher name (manual 10-B), so both go in. The columns can
    be changed via `options.neis` (`NeisFormat`).

    A parallel group puts several courses in one cell, so by default all are
    written (e.g. `Tech/Kim;HomeEc/Lee`) — not losing information is the default;
    if NEIS reads only one subject per cell, `NeisFormat(parallel_separator=None)`
    keeps a single representative.
    """
    options = options or ExportOptions()
    fmt = options.neis
    subject_names = {s.id: s.name for s in problem.subjects}
    teacher_names = {t.id: t.name for t in problem.teachers}
    files: dict[str, bytes] = {}

    # Weekdays missing from the calendar (Sat/Sun) are still written as empty
    # columns up to fmt.day_count — for when NEIS expects a fixed column count.
    day_indexes = range(fmt.day_count)

    for group in problem.class_groups:
        grid = _grid(problem, _class_lessons(timetable, group.id))
        buffer = io.StringIO()
        writer = csv.writer(buffer, delimiter=fmt.delimiter, lineterminator="\r\n")

        if fmt.include_header:
            header = ["교시"]  # period
            for day in day_indexes:
                header.append(DAY_LABELS[day])
                if fmt.teacher_column:
                    header.append(f"{DAY_LABELS[day]}{fmt.teacher_header_suffix}")
            writer.writerow(header)

        for period in range(1, problem.calendar.max_periods + 1):
            row = [fmt.period_label(period)]
            for day in day_indexes:
                # Weekday is an IntEnum, so it looks up integer keys directly.
                # Parallel sections (H5) put several courses in one class and time.
                # Handle them explicitly via `parallel_separator` instead of silently keeping the last.
                items = grid.get((day, period), [])
                if fmt.parallel_separator is None:
                    items = items[:1]
                join = fmt.parallel_separator or ""
                subjects = [
                    options.subject_label(lesson.subject_id, subject_names.get(lesson.subject_id, ""))
                    for lesson in items
                ]
                teachers = [",".join(teacher_names.get(t, t) for t in lesson.teacher_ids) for lesson in items]
                if fmt.teacher_column:
                    row.extend([join.join(subjects), join.join(teachers)])
                else:
                    row.append(
                        join.join(
                            f"{subject}{fmt.teacher_separator}{teacher}" if teacher else subject
                            for subject, teacher in zip(subjects, teachers, strict=True)
                        )
                    )
            writer.writerow(row)

        # The name rule Comcigan uses: 기초시간표(<school year>-<semester>,<grade>-<class>).csv
        number = _class_number(group.name, group.id)
        name = f"기초시간표({options.school_year}-{options.semester},{group.grade}-{number}).csv"
        text = buffer.getvalue()
        try:
            files[name] = text.encode(fmt.encoding)
        except UnicodeEncodeError, LookupError:
            files[name] = text.encode("utf-8-sig")
    return files


def _class_number(name: str, fallback: str) -> str:
    import re

    match = re.search(r"(\d+)\s*반", name)  # "<n>반" = class n
    return match.group(1) if match else fallback


# ---------------------------------------------------------------- printable Excel


def timetable_workbook(
    problem: TimetableProblem,
    timetable: Timetable,
    options: ExportOptions | None = None,
) -> bytes:
    """A printable workbook: overall, class, teacher and special-room timetables."""
    from openpyxl import Workbook
    from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
    from openpyxl.utils import get_column_letter

    options = options or ExportOptions()
    subject_names = {s.id: s.name for s in problem.subjects}
    teacher_names = {t.id: t.name for t in problem.teachers}
    room_names = {r.id: r.name for r in problem.rooms}
    days = problem.calendar.days
    max_periods = problem.calendar.max_periods

    workbook = Workbook()
    bold = Font(bold=True)
    center = Alignment(horizontal="center", vertical="center", wrap_text=True)
    thin = Side(style="thin", color="AAAAAA")
    box = Border(left=thin, right=thin, top=thin, bottom=thin)
    head_fill = PatternFill("solid", fgColor="E8EEF7")
    off_fill = PatternFill("solid", fgColor="F2F2F2")

    def header(cell: Any) -> None:
        cell.font = bold
        cell.alignment = center
        cell.fill = head_fill
        cell.border = box

    def label(lesson: ScheduledLesson, *, with_teacher: bool) -> str:
        subject = options.subject_label(lesson.subject_id, subject_names.get(lesson.subject_id, lesson.subject_id))
        if not with_teacher:
            return subject
        who = ",".join(teacher_names.get(t, t) for t in lesson.teacher_ids)
        return f"{subject}\n{who}" if who else subject

    def write_grid(
        sheet: Any,
        top: int,
        title: str,
        lessons: list[ScheduledLesson],
        *,
        cell_text,
        blocked: set[tuple[Weekday, int]] | None = None,
    ) -> int:
        """Draws one title + weekday x period grid and returns the next start row."""
        blocked = blocked or set()
        sheet.cell(row=top, column=1, value=title).font = bold
        for index, day in enumerate(days, start=2):
            header(sheet.cell(row=top + 1, column=index, value=Weekday(day).label))
        header(sheet.cell(row=top + 1, column=1, value="Period"))
        grid = _grid(problem, lessons)
        for period in range(1, max_periods + 1):
            row = top + 1 + period
            header(sheet.cell(row=row, column=1, value=period))
            for index, day in enumerate(days, start=2):
                cell = sheet.cell(row=row, column=index)
                cell.border = box
                cell.alignment = center
                if period > problem.calendar.periods_per_day.get(day, 0) or (day, period) in blocked:
                    cell.fill = off_fill
                    continue
                items = grid.get((day, period), [])
                cell.value = "\n".join(cell_text(item) for item in items)
        return top + max_periods + 3

    # ---------------------------------------------------------- overall timetable
    overview = workbook.active
    overview.title = "Overview"
    overview.cell(row=1, column=1, value="Overall timetable").font = Font(bold=True, size=14)
    header(overview.cell(row=2, column=1, value="Period"))
    header(overview.cell(row=2, column=2, value="Day"))
    for index, group in enumerate(problem.class_groups, start=3):
        header(overview.cell(row=2, column=index, value=group.name))
        overview.column_dimensions[get_column_letter(index)].width = 12
    row = 3
    by_class = {group.id: _grid(problem, _class_lessons(timetable, group.id)) for group in problem.class_groups}
    for day in days:
        for period in range(1, problem.calendar.periods_per_day.get(day, 0) + 1):
            header(overview.cell(row=row, column=1, value=period))
            header(overview.cell(row=row, column=2, value=Weekday(day).label))
            for index, group in enumerate(problem.class_groups, start=3):
                cell = overview.cell(row=row, column=index)
                cell.border = box
                cell.alignment = center
                items = by_class[group.id].get((day, period), [])
                cell.value = "\n".join(label(item, with_teacher=True) for item in items)
                if not items and (day, period) in {(s.day, s.period) for s in group.unavailable}:
                    cell.fill = off_fill
            row += 1
    overview.column_dimensions["A"].width = 6
    overview.column_dimensions["B"].width = 6

    # ---------------------------------------------------------- class timetables
    classes = workbook.create_sheet("Classes")
    classes.column_dimensions["A"].width = 6
    for index in range(2, len(days) + 2):
        classes.column_dimensions[get_column_letter(index)].width = 16
    top = 1
    for group in problem.class_groups:
        top = write_grid(
            classes,
            top,
            group.name,
            _class_lessons(timetable, group.id),
            cell_text=lambda item: label(item, with_teacher=True),
            blocked={(s.day, s.period) for s in group.unavailable},
        )

    # ---------------------------------------------------------- teacher timetables
    teachers_sheet = workbook.create_sheet("Teachers")
    teachers_sheet.column_dimensions["A"].width = 6
    for index in range(2, len(days) + 2):
        teachers_sheet.column_dimensions[get_column_letter(index)].width = 16
    top = 1
    class_names = {c.id: c.name for c in problem.class_groups}
    for teacher in problem.teachers:
        lessons = _teacher_lessons(timetable, teacher.id)
        top = write_grid(
            teachers_sheet,
            top,
            f"{teacher.name} ({len(lessons)} periods a week)",
            lessons,
            cell_text=lambda item: (
                f"{class_names.get(item.class_group_id, item.class_group_id)}\n"
                f"{subject_names.get(item.subject_id, item.subject_id)}"
            ),
            blocked={(s.day, s.period) for s in teacher.unavailable},
        )

    # ---------------------------------------------------------- special-room timetables
    if problem.rooms:
        rooms_sheet = workbook.create_sheet("Special rooms")
        rooms_sheet.column_dimensions["A"].width = 6
        for index in range(2, len(days) + 2):
            rooms_sheet.column_dimensions[get_column_letter(index)].width = 16
        top = 1
        for room in problem.rooms:
            lessons = [lesson for lesson in timetable.lessons if lesson.room_id == room.id]
            top = write_grid(
                rooms_sheet,
                top,
                f"{room.name} ({room.room_type}) — {len(lessons)} periods a week",
                lessons,
                cell_text=lambda item: (
                    f"{class_names.get(item.class_group_id, item.class_group_id)}\n"
                    f"{subject_names.get(item.subject_id, item.subject_id)}"
                ),
            )

    # ---------------------------------------------------------- teacher load summary
    summary = workbook.create_sheet("Teacher load")
    for index, title in enumerate(["Teacher", "Subject", "Weekly periods", *[Weekday(d).label for d in days]], start=1):
        header(summary.cell(row=1, column=index, value=title))
    for offset, teacher in enumerate(problem.teachers, start=2):
        lessons = _teacher_lessons(timetable, teacher.id)
        per_day = {day: sum(1 for item in lessons if item.slot.day == day) for day in days}
        summary.cell(row=offset, column=1, value=teacher.name)
        summary.cell(
            row=offset, column=2, value=", ".join(sorted(subject_names.get(s, s) for s in teacher.subject_ids))
        )
        summary.cell(row=offset, column=3, value=len(lessons))
        for index, day in enumerate(days, start=4):
            summary.cell(row=offset, column=index, value=per_day[day])
    summary.column_dimensions["A"].width = 12
    summary.column_dimensions["B"].width = 20

    _ = room_names  # names are looked up inside write_grid
    buffer = io.BytesIO()
    workbook.save(buffer)
    return buffer.getvalue()


def neis_zip(
    problem: TimetableProblem,
    timetable: Timetable,
    options: ExportOptions | None = None,
) -> bytes:
    """The NEIS CSV bundle as one zip."""
    import zipfile

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, content in neis_bundle(problem, timetable, options).items():
            archive.writestr(name, content)
    return buffer.getvalue()


__all__ = ["ExportOptions", "NeisFormat", "neis_bundle", "neis_zip", "timetable_workbook"]
