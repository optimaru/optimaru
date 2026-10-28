"""편성 결과 내보내기 — NEIS 일괄파일(CSV)과 엑셀 인쇄.

컴시간의 [인쇄] 메뉴에 대응한다.

- `neis_bundle()`   : [NEIS용 일괄파일]. 학급마다 CSV 한 장.
- `timetable_workbook()` : [전체시간표/학급시간표/교사시간표/특별실시간표] 엑셀.

NEIS 일괄파일은 학급별 `기초시간표(학년도-학기,학년-반).csv` 이름 규칙과
요일 x 교시 격자라는 점이 매뉴얼에 나와 있다. 셀 안의 표기(과목명/교사명)는
NEIS 의 [개설과목 List] 에 등록된 이름과 정확히 같아야 대상란이 채워지므로,
`official_subject_names` 로 단축과목명 대신 정식과목명을 쓸 수 있게 열어 둔다.

CSV 의 세부 구조(헤더 유무, 요일 열 개수, 과목과 교사를 한 칸에 쓰는지)는 매뉴얼에
나오지 않아 기본값이 스크린샷에서 역추론한 값이다 — 실제 NEIS 와 어긋나면 코드가
아니라 `NeisFormat` 만 바꾼다. 자세한 것은 `NeisFormat` docstring 참고.
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

DAY_LABELS = ("월요일", "화요일", "수요일", "목요일", "금요일", "토요일", "일요일")


@dataclass(frozen=True)
class NeisFormat:
    """NEIS 기초시간표 CSV 의 세부 형식.

    매뉴얼 10장은 파일을 **만드는 법과 올리는 법**만 보여준다 — 파일명 규칙
    (`기초시간표(학년도-학기,학년-반).csv`)과 요일 x 교시 격자, 그리고 과목명·교사명이
    둘 다 대조된다는 것까지가 확인된 사실이고, CSV 내부 구조(헤더 유무, 열 개수,
    과목과 교사를 한 칸에 쓰는지)는 나오지 않는다.

    그래서 기본값은 매뉴얼 스크린샷에서 역추론한 값이고, 실제 NEIS 가져오기로
    확인한 뒤 **코드를 고치지 않고** 맞출 수 있도록 열어 둔다. 확인 방법은
    NEIS [학교교육과정]-[시간표 관리]-[기초시간표 관리] -> 조회 -> [가져오기] ->
    [파일에서] 로 올려 보고 `원본` 행이 채워지는지 보는 것이다. `원본` 이 채워지면
    형식은 맞은 것이고, `대상`만 비면 이름 매칭 문제(매뉴얼 10-나)다.
    """

    #: 요일 열 개수. 기본 5(월~금). NEIS 화면은 월~일 7열이라 7도 쓸 수 있다 —
    #: 늘리면 수업이 없는 요일은 빈 열로 채운다.
    day_count: int = 5

    #: `교시,월요일,...` 헤더 행을 넣을지.
    include_header: bool = True

    #: 첫 열 표기. `"교시"` 면 `1교시`, `""` 면 `1`.
    period_suffix: str = "교시"

    #: True 면 요일마다 과목 열과 교사 열을 따로 둔다.
    #: False 면 한 칸에 `과목{teacher_separator}교사` 로 쓴다.
    teacher_column: bool = False
    teacher_separator: str = "/"

    #: 교사 열을 따로 둘 때의 헤더 접미사 (`월요일 교사`).
    teacher_header_suffix: str = " 교사"

    #: 분반/동시수업으로 한 칸에 강좌가 여러 개 올 때의 처리.
    #: 구분자를 주면 `기술/김교사;가정/이교사` 처럼 모두 이어 쓰고, `None` 이면
    #: 첫 강좌 하나만 쓴다 — NEIS 가 한 칸에서 과목 하나만 읽는다면 `None` 으로.
    parallel_separator: str | None = ";"

    delimiter: str = ","

    #: NEIS 는 한글 CSV 를 EUC-KR 계열로 받는다. 이 인코딩으로 못 쓰는 글자가
    #: 있으면 `utf-8-sig` 로 떨어뜨린다.
    encoding: str = "cp949"

    def __post_init__(self) -> None:
        if not 1 <= self.day_count <= len(DAY_LABELS):
            raise ValueError(f"day_count 는 1~{len(DAY_LABELS)} 여야 합니다: {self.day_count}")

    def period_label(self, period: int) -> str:
        return f"{period}{self.period_suffix}"


@dataclass(frozen=True)
class ExportOptions:
    school_year: int = 0
    semester: int = 1
    #: 단축과목명 -> NEIS 정식과목명. 없으면 단축과목명을 그대로 쓴다.
    official_subject_names: dict[str, str] | None = None
    #: NEIS CSV 세부 형식. 실제 NEIS 와 맞지 않으면 여기만 바꾼다.
    neis: NeisFormat = field(default_factory=NeisFormat)

    def subject_label(self, subject_id: str, fallback: str) -> str:
        if self.official_subject_names:
            return self.official_subject_names.get(subject_id, fallback)
        return fallback


# ---------------------------------------------------------------- 격자 만들기


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
    """학급별 NEIS 기초시간표 CSV. `{파일명: 내용}` 을 돌려준다.

    기본 형식은 셀 하나에 `과목명/교사명` 이다. NEIS 가 과목명과 교사명을 모두
    대조하므로(매뉴얼 10-나) 둘 다 넣는다. 열 구성은 `options.neis`(`NeisFormat`)
    로 바꿀 수 있다.

    분반은 한 칸에 강좌가 여럿이라 기본값은 `기술/김교사;가정/이교사` 처럼 모두
    적는다 — 정보를 흘리지 않는 쪽이 기본이고, NEIS 가 한 칸에서 과목 하나만
    읽는다면 `NeisFormat(parallel_separator=None)` 로 대표 하나만 남긴다.
    """
    options = options or ExportOptions()
    fmt = options.neis
    subject_names = {s.id: s.name for s in problem.subjects}
    teacher_names = {t.id: t.name for t in problem.teachers}
    files: dict[str, bytes] = {}

    # 캘린더에 없는 요일(토/일)도 fmt.day_count 만큼은 빈 열로 채운다 — NEIS 가
    # 열 개수를 고정으로 기대할 때를 위해서다.
    day_indexes = range(fmt.day_count)

    for group in problem.class_groups:
        grid = _grid(problem, _class_lessons(timetable, group.id))
        buffer = io.StringIO()
        writer = csv.writer(buffer, delimiter=fmt.delimiter, lineterminator="\r\n")

        if fmt.include_header:
            header = ["교시"]
            for day in day_indexes:
                header.append(DAY_LABELS[day])
                if fmt.teacher_column:
                    header.append(f"{DAY_LABELS[day]}{fmt.teacher_header_suffix}")
            writer.writerow(header)

        for period in range(1, problem.calendar.max_periods + 1):
            row = [fmt.period_label(period)]
            for day in day_indexes:
                # Weekday 는 IntEnum 이라 정수 키로 그대로 조회된다.
                # 분반(H5)은 같은 학급·같은 시간에 강좌가 여러 개다. 조용히 마지막
                # 하나만 남기지 않도록 `parallel_separator` 대로 명시적으로 처리한다.
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

        # 컴시간이 만드는 이름 규칙: 기초시간표(학년도-학기,학년-반).csv
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

    match = re.search(r"(\d+)\s*반", name)
    return match.group(1) if match else fallback


# ---------------------------------------------------------------- 엑셀 인쇄


def timetable_workbook(
    problem: TimetableProblem,
    timetable: Timetable,
    options: ExportOptions | None = None,
) -> bytes:
    """[전체시간표] [학급시간표] [교사시간표] [특별실시간표] 4장짜리 엑셀."""
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
        """제목 + 요일x교시 격자 한 판을 그리고 다음 시작 행을 돌려준다."""
        blocked = blocked or set()
        sheet.cell(row=top, column=1, value=title).font = bold
        for index, day in enumerate(days, start=2):
            header(sheet.cell(row=top + 1, column=index, value=DAY_LABELS[day][0]))
        header(sheet.cell(row=top + 1, column=1, value="교시"))
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

    # ---------------------------------------------------------- 전체시간표
    overview = workbook.active
    overview.title = "전체시간표"
    overview.cell(row=1, column=1, value="전체 시간표").font = Font(bold=True, size=14)
    header(overview.cell(row=2, column=1, value="교시"))
    header(overview.cell(row=2, column=2, value="요일"))
    for index, group in enumerate(problem.class_groups, start=3):
        header(overview.cell(row=2, column=index, value=group.name))
        overview.column_dimensions[get_column_letter(index)].width = 12
    row = 3
    by_class = {group.id: _grid(problem, _class_lessons(timetable, group.id)) for group in problem.class_groups}
    for day in days:
        for period in range(1, problem.calendar.periods_per_day.get(day, 0) + 1):
            header(overview.cell(row=row, column=1, value=period))
            header(overview.cell(row=row, column=2, value=DAY_LABELS[day][0]))
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

    # ---------------------------------------------------------- 학급시간표
    classes = workbook.create_sheet("학급시간표")
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

    # ---------------------------------------------------------- 교사시간표
    teachers_sheet = workbook.create_sheet("교사시간표")
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
            f"{teacher.name} 선생님 (주 {len(lessons)}시간)",
            lessons,
            cell_text=lambda item: (
                f"{class_names.get(item.class_group_id, item.class_group_id)}\n"
                f"{subject_names.get(item.subject_id, item.subject_id)}"
            ),
            blocked={(s.day, s.period) for s in teacher.unavailable},
        )

    # ---------------------------------------------------------- 특별실시간표
    if problem.rooms:
        rooms_sheet = workbook.create_sheet("특별실시간표")
        rooms_sheet.column_dimensions["A"].width = 6
        for index in range(2, len(days) + 2):
            rooms_sheet.column_dimensions[get_column_letter(index)].width = 16
        top = 1
        for room in problem.rooms:
            lessons = [lesson for lesson in timetable.lessons if lesson.room_id == room.id]
            top = write_grid(
                rooms_sheet,
                top,
                f"{room.name} ({room.room_type}) — 주 {len(lessons)}시간",
                lessons,
                cell_text=lambda item: (
                    f"{class_names.get(item.class_group_id, item.class_group_id)}\n"
                    f"{subject_names.get(item.subject_id, item.subject_id)}"
                ),
            )

    # ---------------------------------------------------------- 교사별 시수 요약
    summary = workbook.create_sheet("교사시수")
    for index, title in enumerate(["교사", "과목", "주간시수", *[DAY_LABELS[d][0] for d in days]], start=1):
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

    _ = room_names  # 이름 조회는 write_grid 안에서 처리한다
    buffer = io.BytesIO()
    workbook.save(buffer)
    return buffer.getvalue()


def neis_zip(
    problem: TimetableProblem,
    timetable: Timetable,
    options: ExportOptions | None = None,
) -> bytes:
    """NEIS CSV 묶음을 zip 한 장으로."""
    import zipfile

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, content in neis_bundle(problem, timetable, options).items():
            archive.writestr(name, content)
    return buffer.getvalue()


__all__ = ["ExportOptions", "NeisFormat", "neis_bundle", "neis_zip", "timetable_workbook"]
