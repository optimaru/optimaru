"""Comcigan-format teacher load workbook (Excel) -> TimetableProblem.

Reads the workbook layout that Comcigan's (컴시간, a timetabling tool widely
used in Korean schools) "import teacher load table" accepts, as is. The goal is
to let a school upload the file it already uses, so the only mandatory load
sheet is Comcigan's own `교사별시수표` (teacher load table); what Comcigan
collects on separate screens (periods per weekday / basics / subject register /
special rooms / special operations) comes in optional sheets.

The sheet names, column headers and keywords below are the Korean labels of
that file format and are matched against the input verbatim.

Sheets
------
- `기본사항`     (basics, required) school name / school year / classes per grade
- `요일별시수`   (periods per weekday, required) grade x weekday -> periods
- `교사별시수표` (teacher load table, required) Comcigan's original layout
- `과목`         (subjects, optional) special room / co-teaching / preferred and
                 avoided periods per short subject name
- `특별실`       (special rooms, optional) room name + type
- `특별작업`     (special operations, optional) consecutive lessons / forbidden
                 placements / fixed placements / parallel sections

Periods per weekday per grade
-----------------------------
The domain's `SchoolCalendar` is school-wide, so it cannot hold different
periods per weekday for each grade. The calendar therefore holds the
**maximum** periods per weekday, and the extra cells of shorter grades are
blocked with their classes' `ClassGroup.unavailable` — for the solver that
means "this class has no lesson then", and the contract (API_SPEC) stays as is.
"""

from __future__ import annotations

import re
from collections import defaultdict
from dataclasses import dataclass, field
from typing import IO, Any

from teacher_scheduling.domain.entities import ClassGroup, Course, Room, Subject, Teacher, TimeSlot
from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.domain.problem import SchoolCalendar, TimetableProblem

SHEET_BASICS = "기본사항"  # basics
SHEET_DAILY = "요일별시수"  # periods per weekday
SHEET_LOAD = "교사별시수표"  # teacher load table
SHEET_SUBJECTS = "과목"  # subjects
SHEET_ROOMS = "특별실"  # special rooms
SHEET_SPECIAL = "특별작업"  # special operations

# Comcigan writes weekdays as one Korean syllable: 월 Mon, 화 Tue, 수 Wed, 목 Thu, 금 Fri, 토 Sat.
_DAY_BY_LABEL = dict(zip("월화수목금토", Weekday, strict=True))
_GRADE_RE = re.compile(r"(\d+)\s*학년")  # "<n>학년" = grade n
# Accepts "월1", "월 1교시" and "월요일 1교시" (Mon 1 / Mon period 1 / Monday period 1).
_SLOT_RE = re.compile(r"^\s*([월화수목금토])\s*(?:요일)?\s*(\d+)\s*(?:교시)?\s*$")


class ExcelFormatError(ValueError):
    """The workbook does not match the contract. Carries a message to show the user as is."""


@dataclass
class SchoolBasics:
    """School metadata, matching Comcigan's "basics" screen."""

    school_name: str = ""
    school_year: int | None = None
    classes_per_grade: dict[int, int] = field(default_factory=dict)


@dataclass
class ImportResult:
    """Import result. `problem` is the payload; the rest is extra information shown on screen as is."""

    problem: TimetableProblem
    basics: SchoolBasics
    # short subject name -> official subject names (used by the NEIS export)
    official_subject_names: dict[str, list[str]] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


# ---------------------------------------------------------------- cell helpers


def _norm(value: Any) -> str:
    """A cell value as a comparable string. Absorbs None, blanks and numbers."""
    if value is None:
        return ""
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value).strip()


def _as_int(value: Any) -> int | None:
    text = _norm(value)
    if not text:
        return None
    try:
        return int(float(text))
    except ValueError:
        return None


def _parse_slot(text: str) -> TimeSlot:
    match = _SLOT_RE.match(text)
    if not match:
        raise ExcelFormatError(f"cannot read the time notation: {text!r} (e.g. '월1' or '월 1교시', i.e. Monday period 1)")
    day_label, period = match.groups()
    return TimeSlot(day=_DAY_BY_LABEL[day_label], period=int(period))


def _parse_periods(text: str) -> frozenset[int]:
    """Periods written as '1,2,3' or '1-4'."""
    if not text:
        return frozenset()
    periods: set[int] = set()
    for chunk in re.split(r"[,\s]+", text):
        if not chunk:
            continue
        if "-" in chunk:
            start, _, end = chunk.partition("-")
            periods.update(range(int(start), int(end) + 1))
        else:
            periods.add(int(chunk))
    return frozenset(periods)


def _rows(sheet: Any) -> list[list[Any]]:
    return [list(row) for row in sheet.iter_rows(values_only=True)]


# ---------------------------------------------------------------- sheet parsers


def _read_basics(rows: list[list[Any]]) -> SchoolBasics:
    """Two columns, `item | value`. Classes per grade are written as 'N학년 학급수' (classes in grade N)."""
    basics = SchoolBasics()
    for row in rows:
        if not row:
            continue
        key, value = _norm(row[0]), (row[1] if len(row) > 1 else None)
        if not key:
            continue
        if key == "학교명":  # school name
            basics.school_name = _norm(value)
        elif key in ("학년도", "학년"):  # school year
            basics.school_year = _as_int(value)
        elif (match := _GRADE_RE.search(key)) and "학급수" in key:  # classes in grade N
            count = _as_int(value)
            if count:
                basics.classes_per_grade[int(match.group(1))] = count
    if not basics.classes_per_grade:
        raise ExcelFormatError(f"[{SHEET_BASICS}] no 'N학년 학급수' (classes in grade N) found in the sheet")
    return basics


def _read_daily_periods(rows: list[list[Any]]) -> dict[int, dict[Weekday, int]]:
    """The periods-per-weekday sheet. The first row is the weekday header, then one row of periods per grade."""
    header: list[Weekday | None] = []
    per_grade: dict[int, dict[Weekday, int]] = {}
    for row in rows:
        if not row:
            continue
        label = _norm(row[0])
        if not header:
            # Header row: remember the columns holding the Mon–Sat labels.
            # Weekday.MON is 0 and therefore falsy — compare with `is not None` so
            # that a sheet with only Monday is still recognised as a header.
            header = [_DAY_BY_LABEL.get(_norm(cell)) for cell in row]
            if not any(day is not None for day in header):
                header = []
            continue
        match = _GRADE_RE.search(label)
        if not match:
            continue
        grade = int(match.group(1))
        days: dict[Weekday, int] = {}
        for index, day in enumerate(header):
            if day is None or index >= len(row):
                continue
            periods = _as_int(row[index])
            if periods:
                days[day] = periods
        if days:
            per_grade[grade] = days
    if not per_grade:
        raise ExcelFormatError(f"[{SHEET_DAILY}] no periods per weekday per grade found in the sheet")
    return per_grade


def _read_subject_meta(rows: list[list[Any]]) -> dict[str, dict[str, Any]]:
    """The subjects sheet (optional). Columns are found by header name."""
    if not rows:
        return {}
    header = {_norm(cell): index for index, cell in enumerate(rows[0])}
    if "단축과목명" not in header:  # short subject name
        return {}

    def cell(row: list[Any], name: str) -> str:
        index = header.get(name)
        return _norm(row[index]) if index is not None and index < len(row) else ""

    meta: dict[str, dict[str, Any]] = {}
    for row in rows[1:]:
        short = cell(row, "단축과목명")  # short subject name
        if not short:
            continue
        meta[short] = {
            "required_room_type": cell(row, "특별실") or None,  # special room
            # 복수교사 = co-teaching; "복수" = yes
            "team_teaching": cell(row, "복수교사") in ("O", "o", "복수", "Y", "y", "TRUE", "True", "1"),
            "preferred_periods": _parse_periods(cell(row, "선호교시")),  # preferred periods
            "avoided_periods": _parse_periods(cell(row, "기피교시")),  # avoided periods
        }
    return meta


def _read_rooms(rows: list[list[Any]]) -> list[Room]:
    """The special-rooms sheet (optional). `room name | type`."""
    rooms: list[Room] = []
    for index, row in enumerate(rows):
        if not row:
            continue
        name, room_type = _norm(row[0]), _norm(row[1]) if len(row) > 1 else ""
        if not name or name in ("특별실명", "실명"):  # header labels: room name
            continue
        if not room_type:
            raise ExcelFormatError(f"[{SHEET_ROOMS}] {name}: the type is empty")
        rooms.append(Room(id=f"R{index:02d}", name=name, room_type=room_type))
    return rooms


@dataclass
class _LoadRow:
    """One teacher-load-table row: the periods per class of one (official subject, short subject, teacher)."""

    official: str
    short: str
    teacher: str
    hours: dict[tuple[int, int], int]  # (grade, class number) -> periods


def _read_load_table(rows: list[list[Any]]) -> tuple[list[_LoadRow], dict[int, int]]:
    """Reads Comcigan's original layout; returns the rows and the highest class number per grade.

    The header has two rows — grade labels ('1학년/2학년/3학년', merged cells) on top,
    class numbers below. openpyxl gives a merged cell's value only to its first
    cell, so the value is carried rightwards.
    """
    header_index = next(
        # 정식과목명 = official subject name
        (index for index, row in enumerate(rows) if any(_norm(cell) == "정식과목명" for cell in row)),
        None,
    )
    if header_index is None:
        raise ExcelFormatError(f"[{SHEET_LOAD}] no '정식과목명' (official subject name) header found in the sheet")

    header = rows[header_index]
    columns = {_norm(cell): index for index, cell in enumerate(header)}
    # official subject name, short subject name, teacher name
    for required in ("정식과목명", "단축과목명", "교사명"):
        if required not in columns:
            raise ExcelFormatError(f"[{SHEET_LOAD}] the header has no '{required}' column")

    # Carry the grade labels rightwards (merged-cell fix) and pair them with the class numbers below.
    grade_of_column: dict[int, int] = {}
    current: int | None = None
    for index, cell in enumerate(header):
        if match := _GRADE_RE.search(_norm(cell)):
            current = int(match.group(1))
        if current is not None and index > columns["교사명"]:  # teacher name
            grade_of_column[index] = current

    class_row = rows[header_index + 1] if header_index + 1 < len(rows) else []
    class_of_column: dict[int, tuple[int, int]] = {}
    max_class: dict[int, int] = defaultdict(int)
    for index, cell in enumerate(class_row):
        grade = grade_of_column.get(index)
        number = _as_int(cell)
        if grade is None or number is None:
            continue
        class_of_column[index] = (grade, number)
        max_class[grade] = max(max_class[grade], number)
    if not class_of_column:
        raise ExcelFormatError(f"[{SHEET_LOAD}] cannot read the grade/class header (class numbers are needed under the grade row)")

    order_column = columns.get("순")  # row number
    entries: list[_LoadRow] = []
    for row in rows[header_index + 2 :]:
        if not row:
            continue
        # Comcigan rule: the data ends at the first row without a row number.
        if order_column is not None and _as_int(row[order_column] if order_column < len(row) else None) is None:
            break
        official = _norm(row[columns["정식과목명"]]) if columns["정식과목명"] < len(row) else ""
        short = _norm(row[columns["단축과목명"]]) if columns["단축과목명"] < len(row) else ""
        teacher = _norm(row[columns["교사명"]]) if columns["교사명"] < len(row) else ""
        if not (short and teacher):
            continue
        hours = {
            key: value
            for index, key in class_of_column.items()
            if index < len(row) and (value := _as_int(row[index])) and value > 0
        }
        if hours:
            entries.append(_LoadRow(official=official or short, short=short, teacher=teacher, hours=hours))
    if not entries:
        raise ExcelFormatError(f"[{SHEET_LOAD}] there is not a single row of load data")
    return entries, dict(max_class)


#: Kinds accepted in the special-operations sheet; any other value ends the table.
#: 배정금지 forbidden placement, 이동금지 no move, 특별교사 special teacher,
#: 연속수업 consecutive lessons, 고정배정 fixed placement, 분반 / 동시수업 parallel sections.
SPECIAL_KINDS = frozenset({"배정금지", "이동금지", "특별교사", "연속수업", "고정배정", "분반", "동시수업"})


def _read_special(rows: list[list[Any]]) -> list[tuple[str, str, str]]:
    """The special-operations sheet (optional). Three columns `kind | target | value`.

    Stops at the first empty row — the layout keeps usage notes below the table,
    and those must not be read as data.
    """
    result = []
    for row in rows:
        kind = _norm(row[0]) if row else ""
        if not kind:
            if result:
                break
            continue
        if kind == "구분":  # header label: kind
            continue
        if kind not in SPECIAL_KINDS:
            break
        target = _norm(row[1]) if len(row) > 1 else ""
        value = _norm(row[2]) if len(row) > 2 else ""
        result.append((kind, target, value))
    return result


# ---------------------------------------------------------------- assembly


def _class_id(grade: int, number: int) -> str:
    return f"C{grade}{number:02d}"


def _build_class_groups(
    basics: SchoolBasics,
    daily: dict[int, dict[Weekday, int]],
    calendar: SchoolCalendar,
) -> list[ClassGroup]:
    """Blocks the extra cells as class unavailability when a grade has fewer periods per weekday than the school maximum."""
    groups = []
    for grade in sorted(basics.classes_per_grade):
        grade_days = daily.get(grade, {})
        blocked = frozenset(
            TimeSlot(day=day, period=period)
            for day, total in calendar.periods_per_day.items()
            for period in range(grade_days.get(day, 0) + 1, total + 1)
        )
        for number in range(1, basics.classes_per_grade[grade] + 1):
            groups.append(
                ClassGroup(
                    id=_class_id(grade, number),
                    # Comcigan's class name ("<grade>학년 <n>반" = grade g, class n): the special-operations
                    # sheet and the NEIS export refer to classes by this exact name.
                    name=f"{grade}학년 {number}반",
                    grade=grade,
                    unavailable=blocked,
                )
            )
    return groups


def _apply_special(
    operations: list[tuple[str, str, str]],
    *,
    teacher_id_by_name: dict[str, str],
    course_index: dict[tuple[str, str], list[str]],
    unavailable: dict[str, set[TimeSlot]],
    block_periods: dict[str, int],
    fixed_slots: dict[str, set[TimeSlot]],
    parallel: dict[str, str],
    subject_ids: set[str],
    warnings: list[str],
) -> None:
    """Moves the special-operations sheet into domain fields."""
    for kind, target, value in operations:
        if kind in ("배정금지", "이동금지", "특별교사"):  # forbidden placement / no move / special teacher
            teacher_id = teacher_id_by_name.get(target)
            if teacher_id is None:
                warnings.append(f"[{SHEET_SPECIAL}] forbidden placement: teacher '{target}' not found in the load table")
                continue
            unavailable[teacher_id].add(_parse_slot(value))
        elif kind == "연속수업":  # consecutive lessons
            if target not in subject_ids:
                warnings.append(f"[{SHEET_SPECIAL}] consecutive lessons: no subject '{target}'")
                continue
            size = _as_int(value) or 2
            for (_, subject_id), course_ids in course_index.items():
                if subject_id == target:
                    for course_id in course_ids:
                        block_periods[course_id] = size
        elif kind == "고정배정":  # fixed placement
            class_name, _, subject_name = target.partition("/")
            course_ids = course_index.get((class_name.strip(), subject_name.strip()), [])
            if not course_ids:
                warnings.append(f"[{SHEET_SPECIAL}] fixed placement: course '{target}' not found")
                continue
            for course_id in course_ids:
                fixed_slots[course_id].add(_parse_slot(value))
        elif kind in ("분반", "동시수업"):  # parallel sections
            class_name, _, subject_name = target.partition("/")
            course_ids = course_index.get((class_name.strip(), subject_name.strip()), [])
            if not course_ids:
                warnings.append(f"[{SHEET_SPECIAL}] parallel sections: course '{target}' not found")
                continue
            for course_id in course_ids:
                parallel[course_id] = value or target
        else:
            warnings.append(f"[{SHEET_SPECIAL}] unknown kind: {kind}")


def build_problem(
    *,
    basics: SchoolBasics,
    daily: dict[int, dict[Weekday, int]],
    load_rows: list[_LoadRow],
    subject_meta: dict[str, dict[str, Any]],
    rooms: list[Room],
    special: list[tuple[str, str, str]],
) -> ImportResult:
    """Assembles the parsed sheets into one `TimetableProblem`."""
    warnings: list[str] = []

    # Calendar: per weekday, the longest period count over the grades.
    calendar_days: dict[Weekday, int] = defaultdict(int)
    for grade_days in daily.values():
        for day, periods in grade_days.items():
            calendar_days[day] = max(calendar_days[day], periods)
    calendar = SchoolCalendar(periods_per_day=dict(calendar_days))

    class_groups = _build_class_groups(basics, daily, calendar)
    class_ids = {group.id for group in class_groups}
    class_id_by_name = {group.name: group.id for group in class_groups}

    # Subjects: the short subject name is the scheduling unit (several official names map to one short name).
    official_names: dict[str, list[str]] = defaultdict(list)
    for row in load_rows:
        if row.official not in official_names[row.short]:
            official_names[row.short].append(row.official)
    subjects = [
        Subject(
            id=short,
            name=short,
            required_room_type=subject_meta.get(short, {}).get("required_room_type"),
            preferred_periods=subject_meta.get(short, {}).get("preferred_periods", frozenset()),
            avoided_periods=subject_meta.get(short, {}).get("avoided_periods", frozenset()),
        )
        for short in official_names
    ]
    subject_ids = {subject.id for subject in subjects}

    room_types = {room.room_type for room in rooms}
    for subject in subjects:
        if subject.required_room_type and subject.required_room_type not in room_types:
            raise ExcelFormatError(
                f"[{SHEET_SUBJECTS}] special room '{subject.required_room_type}' required by '{subject.id}'"
                f" is missing from the [{SHEET_ROOMS}] sheet"
            )

    # Teachers: T01.. in order of first appearance.
    teacher_names = list(dict.fromkeys(row.teacher for row in load_rows))
    teacher_id_by_name = {name: f"T{index:02d}" for index, name in enumerate(teacher_names, start=1)}
    teaches: dict[str, set[str]] = defaultdict(set)
    for row in load_rows:
        teaches[teacher_id_by_name[row.teacher]].add(row.short)

    # Courses: periods summed per (class, short subject, teacher).
    #   Official subjects scattered over one cell (e.g. 수학7-가/수학7-나) become one course.
    merged: dict[tuple[str, str, str], int] = defaultdict(int)
    for row in load_rows:
        teacher_id = teacher_id_by_name[row.teacher]
        for (grade, number), hours in row.hours.items():
            class_id = _class_id(grade, number)
            if class_id not in class_ids:
                warnings.append(
                    f"[{SHEET_LOAD}] {row.short}/{row.teacher}: grade {grade} class {number}"
                    f" is beyond the class count in [{SHEET_BASICS}] and is ignored"
                )
                continue
            merged[class_id, row.short, teacher_id] += hours

    # A co-taught subject merges the teachers of the same (class, subject) into one course.
    by_class_subject: dict[tuple[str, str], list[tuple[str, int]]] = defaultdict(list)
    for (class_id, short, teacher_id), hours in merged.items():
        by_class_subject[class_id, short].append((teacher_id, hours))

    unavailable: dict[str, set[TimeSlot]] = defaultdict(set)
    block_periods: dict[str, int] = {}
    fixed_slots: dict[str, set[TimeSlot]] = defaultdict(set)
    parallel: dict[str, str] = {}
    course_specs: list[tuple[str, str, str, tuple[str, ...], int]] = []
    course_index: dict[tuple[str, str], list[str]] = defaultdict(list)

    for (class_id, short), assignments in sorted(by_class_subject.items()):
        team = subject_meta.get(short, {}).get("team_teaching", False)
        if team and len({hours for _, hours in assignments}) > 1:
            warnings.append(
                f"[{SHEET_LOAD}] co-taught subject {short}/{class_id}: teachers' periods differ; using the minimum"
            )
        if team:
            hours = min(hours for _, hours in assignments)
            teacher_ids = tuple(sorted(teacher_id for teacher_id, _ in assignments))
            course_id = f"{class_id}-{short}"
            course_specs.append((course_id, class_id, short, teacher_ids, hours))
            course_index[class_id, short].append(course_id)
        else:
            for index, (teacher_id, hours) in enumerate(sorted(assignments)):
                suffix = "" if len(assignments) == 1 else f"-{index + 1}"
                course_id = f"{class_id}-{short}{suffix}"
                course_specs.append((course_id, class_id, short, (teacher_id,), hours))
                course_index[class_id, short].append(course_id)

    # The special-operations sheet names classes and subjects in human-readable form, so build a name index too.
    named_index: dict[tuple[str, str], list[str]] = defaultdict(list)
    for (class_id, short), course_ids in course_index.items():
        class_name = next(group.name for group in class_groups if group.id == class_id)
        named_index[class_name, short].extend(course_ids)
        named_index[class_id, short].extend(course_ids)
    _apply_special(
        special,
        teacher_id_by_name=teacher_id_by_name,
        course_index=named_index,
        unavailable=unavailable,
        block_periods=block_periods,
        fixed_slots=fixed_slots,
        parallel=parallel,
        subject_ids=subject_ids,
        warnings=warnings,
    )

    teachers = [
        Teacher(
            id=teacher_id_by_name[name],
            name=name,
            subject_ids=frozenset(teaches[teacher_id_by_name[name]]),
            unavailable=frozenset(unavailable.get(teacher_id_by_name[name], set())),
        )
        for name in teacher_names
    ]

    courses = []
    for course_id, class_id, short, teacher_ids, hours in course_specs:
        block = block_periods.get(course_id, 0)
        courses.append(
            Course(
                id=course_id,
                class_group_id=class_id,
                subject_id=short,
                teacher_ids=teacher_ids,
                periods_per_week=hours,
                block_periods=min(block, hours),
                # To fit into 5 days, the daily cap must be at least ceil(periods / 5).
                max_periods_per_day=max(1, -(-hours // len(calendar.days))),
                fixed_slots=frozenset(fixed_slots.get(course_id, set())),
                parallel_group=parallel.get(course_id),
            )
        )

    problem = TimetableProblem(
        calendar=calendar,
        subjects=tuple(subjects),
        teachers=tuple(teachers),
        class_groups=tuple(class_groups),
        rooms=tuple(rooms),
        courses=tuple(courses),
    )
    _warn_on_capacity(problem, class_id_by_name, warnings)
    return ImportResult(
        problem=problem,
        basics=basics,
        official_subject_names=dict(official_names),
        warnings=warnings,
    )


def _warn_on_capacity(problem: TimetableProblem, class_id_by_name: dict[str, str], warnings: list[str]) -> None:
    """Early warning matching Comcigan's "periods per weekday total == load table total" check."""
    for group in problem.class_groups:
        available = sum(
            1
            for day, periods in problem.calendar.periods_per_day.items()
            for period in range(1, periods + 1)
            if TimeSlot(day=day, period=period) not in group.unavailable
        )
        assigned = sum(course.periods_per_week for course in problem.courses_by_class_group.get(group.id, ()))
        if assigned > available:
            warnings.append(f"{group.name}: {assigned} periods assigned > {available} cells available — cannot be scheduled")
        elif assigned < available:
            warnings.append(
                f"{group.name}: {assigned} periods assigned < {available} cells available — {available - assigned} cells stay empty"
            )


# ---------------------------------------------------------------- entry point


def import_workbook(source: str | IO[bytes]) -> ImportResult:
    """Reads a workbook (path or binary stream) into a problem."""
    try:
        from openpyxl import load_workbook
    except ImportError as exc:  # pragma: no cover - dependency missing
        raise ExcelFormatError("openpyxl is not installed") from exc

    # .xlsx is a zip, so a non-Excel file blows up with BadZipFile. It is just a
    # wrong upload, so turn it into a format error.
    try:
        workbook = load_workbook(source, data_only=True, read_only=True)
    except ExcelFormatError:
        raise
    except Exception as exc:
        raise ExcelFormatError(f"cannot read the file as an Excel workbook (.xlsx): {exc}") from exc

    try:
        available = set(workbook.sheetnames)
        for required in (SHEET_BASICS, SHEET_DAILY, SHEET_LOAD):
            if required not in available:
                raise ExcelFormatError(f"required sheet '{required}' is missing (sheets present: {sorted(available)})")

        basics = _read_basics(_rows(workbook[SHEET_BASICS]))
        daily = _read_daily_periods(_rows(workbook[SHEET_DAILY]))
        load_rows, max_class = _read_load_table(_rows(workbook[SHEET_LOAD]))
        subject_meta = _read_subject_meta(_rows(workbook[SHEET_SUBJECTS])) if SHEET_SUBJECTS in available else {}
        rooms = _read_rooms(_rows(workbook[SHEET_ROOMS])) if SHEET_ROOMS in available else []
        special = _read_special(_rows(workbook[SHEET_SPECIAL])) if SHEET_SPECIAL in available else []
    finally:
        workbook.close()

    # When the basics sheet has no class counts, infer them from the class numbers in the load table.
    for grade, count in max_class.items():
        basics.classes_per_grade.setdefault(grade, count)

    missing = sorted(set(max_class) - set(daily))
    if missing:
        raise ExcelFormatError(f"[{SHEET_DAILY}] the sheet has no periods per weekday for grade {missing}")

    return build_problem(
        basics=basics,
        daily=daily,
        load_rows=load_rows,
        subject_meta=subject_meta,
        rooms=rooms,
        special=special,
    )


__all__ = [
    "ExcelFormatError",
    "ImportResult",
    "SchoolBasics",
    "import_workbook",
]
