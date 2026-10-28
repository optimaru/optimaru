"""Generates a sample workbook in the Comcigan layout.

Builds an example file in exactly the layout `excel.import_workbook` reads.
A user downloads it as the teacher-load template and overwrites it with the
school's own data; uploaded as is, it is also complete data that solves.
Sheet names, column headers, keywords and weekday notation are the Korean
labels of the Comcigan format; the sample data itself is in English.

School: 3 grades x 3 classes, 32–34 periods a week, 5 special-room types.
"""

from __future__ import annotations

from io import BytesIO
from typing import IO, Any

GRADES = (1, 2, 3)
CLASSES_PER_GRADE = 3

#: Periods per weekday per grade (keys: 월 Mon … 금 Fri). Only grade 3 has one
#: more period on Mon/Tue — shows the per-grade handling.
DAILY_PERIODS: dict[int, dict[str, int]] = {
    1: {"월": 6, "화": 6, "수": 7, "목": 7, "금": 6},
    2: {"월": 6, "화": 6, "수": 7, "목": 7, "금": 6},
    3: {"월": 7, "화": 7, "수": 7, "목": 7, "금": 6},
}

#: (short subject name, official subject name, special room, preferred periods, avoided periods)
SUBJECTS: tuple[tuple[str, str, str, str, str], ...] = (
    ("Korean", "Korean", "", "", ""),
    ("Math", "Math", "", "1-4", ""),
    ("English", "English", "", "", ""),
    ("Science", "Science", "science lab", "", ""),
    ("Social", "Social studies", "", "", ""),
    ("PE", "Physical education", "gym", "", "1"),
    ("Music", "Music", "music room", "", ""),
    ("Art", "Art", "art room", "", ""),
    ("Ethics", "Ethics", "", "", ""),
    ("TechHome", "Technology and Home Economics", "", "", ""),
    ("Info", "Informatics", "computer lab", "", ""),
)

#: Periods per subject per grade (the sum must equal that grade's weekly periods).
CURRICULUM: dict[int, dict[str, int]] = {
    1: {"Korean": 5, "Math": 5, "English": 5, "Science": 4, "Social": 3, "PE": 3, "Music": 2, "Art": 2, "Ethics": 1, "TechHome": 2},
    2: {"Korean": 5, "Math": 5, "English": 5, "Science": 4, "Social": 3, "PE": 3, "Music": 2, "Art": 2, "Ethics": 1, "TechHome": 2},
    3: {
        "Korean": 5,
        "Math": 5,
        "English": 5,
        "Science": 4,
        "Social": 4,
        "PE": 3,
        "Music": 2,
        "Art": 2,
        "Ethics": 1,
        "TechHome": 2,
        "Info": 1,
    },
}

#: subject -> teacher names. Classes are spread evenly below.
TEACHERS: dict[str, tuple[str, ...]] = {
    "Korean": ("Kim Haneul", "Lee Seojun", "Park Jihyun"),
    "Math": ("Choi Minjun", "Jung Yerin", "Kang Doyun"),
    "English": ("Cho Eunbi", "Yoon Taeho", "Jang Woojin"),
    "Science": ("Han Soyul", "Seo Junho"),
    "Social": ("Moon Gayoung", "Bae Siwoo"),
    "PE": ("Oh Jihwan", "Lim Seri"),
    "Music": ("Shin Haram",),
    "Art": ("Hwang Dokyung",),
    "Ethics": ("Kwon Nayun",),
    "TechHome": ("Nam Kihoon",),
    "Info": ("Tak Hyunwoo",),
}

ROOMS: tuple[tuple[str, str], ...] = (
    ("Science lab 1", "science lab"),
    ("Science lab 2", "science lab"),
    ("Gym", "gym"),
    ("Music room", "music room"),
    ("Art room", "art room"),
    ("Computer lab", "computer lab"),
)

SPECIAL_OPERATIONS: tuple[tuple[str, str, str], ...] = (
    # kind (연속수업 consecutive lessons, 배정금지 forbidden placement), target, value
    ("연속수업", "Art", "2"),
    ("배정금지", "Kim Haneul", "월1"),  # 월1 = Monday period 1
    ("배정금지", "Kwon Nayun", "금6"),  # 금6 = Friday period 6
)


def _all_classes() -> list[tuple[int, int]]:
    return [(grade, number) for grade in GRADES for number in range(1, CLASSES_PER_GRADE + 1)]


def _assign_teacher(subject: str, grade: int, number: int) -> str:
    """Deals the classes out to a subject's teachers in turn."""
    names = TEACHERS[subject]
    index = (GRADES.index(grade) * CLASSES_PER_GRADE + (number - 1)) % len(names)
    return names[index]


def build_rows() -> list[tuple[str, str, str, dict[tuple[int, int], int]]]:
    """(official subject name, short subject name, teacher name, {(grade, class): periods}) rows.

    As in Comcigan, only rows with the same (official subject, short subject, teacher) are combined.
    """
    official = {short: full for short, full, *_ in SUBJECTS}
    grouped: dict[tuple[str, str], dict[tuple[int, int], int]] = {}
    for grade, number in _all_classes():
        for subject, hours in CURRICULUM[grade].items():
            teacher = _assign_teacher(subject, grade, number)
            grouped.setdefault((subject, teacher), {})[grade, number] = hours
    return [
        (official[subject], subject, teacher, hours)
        for (subject, teacher), hours in sorted(grouped.items(), key=lambda item: (item[0][0], item[0][1]))
    ]


def write_workbook(destination: str | IO[bytes] | None = None) -> bytes:
    """Builds and saves the sample workbook, and returns its bytes as well."""
    from openpyxl import Workbook
    from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
    from openpyxl.utils import get_column_letter

    workbook = Workbook()
    header_font = Font(bold=True)
    center = Alignment(horizontal="center", vertical="center")
    thin = Side(style="thin", color="BBBBBB")
    box = Border(left=thin, right=thin, top=thin, bottom=thin)
    fill = PatternFill("solid", fgColor="EEF3FB")

    def style_header(cell: Any) -> None:
        cell.font = header_font
        cell.alignment = center
        cell.fill = fill
        cell.border = box

    # ---------------------------------------------------------- basics (기본사항)
    basics = workbook.active
    basics.title = "기본사항"
    basics.column_dimensions["A"].width = 18
    basics.column_dimensions["B"].width = 24
    rows: list[tuple[str, Any]] = [
        ("항목", "값"),  # item, value
        ("학교명", "Comcigan Middle School"),  # school name
        ("학년도", 2026),  # school year
        *[(f"{grade}학년 학급수", CLASSES_PER_GRADE) for grade in GRADES],  # classes in grade N
        ("점심시간 직전교시", 4),  # period right before lunch
    ]
    for index, (key, value) in enumerate(rows, start=1):
        basics.cell(row=index, column=1, value=key)
        basics.cell(row=index, column=2, value=value)
    for column in (1, 2):
        style_header(basics.cell(row=1, column=column))

    # ---------------------------------------------------------- periods per weekday (요일별시수)
    daily = workbook.create_sheet("요일별시수")
    day_names = ["월", "화", "수", "목", "금"]  # Mon … Fri
    daily.cell(row=1, column=1, value="학년")  # grade
    for index, day in enumerate(day_names, start=2):
        daily.cell(row=1, column=index, value=day)
    daily.cell(row=1, column=len(day_names) + 2, value="계")  # total
    for offset, grade in enumerate(GRADES, start=2):
        daily.cell(row=offset, column=1, value=f"{grade}학년")  # grade N
        for index, day in enumerate(day_names, start=2):
            daily.cell(row=offset, column=index, value=DAILY_PERIODS[grade][day])
        daily.cell(row=offset, column=len(day_names) + 2, value=sum(DAILY_PERIODS[grade].values()))
    for column in range(1, len(day_names) + 3):
        style_header(daily.cell(row=1, column=column))
        daily.column_dimensions[get_column_letter(column)].width = 8

    # ---------------------------------------------------------- teacher load table (교사별시수표)
    sheet = workbook.create_sheet("교사별시수표")
    fixed = ["순", "정식과목명", "단축과목명", "교사명"]  # no., official subject, short subject, teacher
    sheet.cell(row=1, column=1, value="교사별 시수표").font = Font(bold=True, size=14)

    # Row 2: fixed columns + grade labels (merged); row 3: class numbers
    for index, label in enumerate(fixed, start=1):
        sheet.cell(row=2, column=index, value=label)
        sheet.merge_cells(start_row=2, start_column=index, end_row=3, end_column=index)
    column = len(fixed) + 1
    for grade in GRADES:
        start = column
        for number in range(1, CLASSES_PER_GRADE + 1):
            sheet.cell(row=3, column=column, value=number)
            column += 1
        sheet.cell(row=2, column=start, value=f"{grade}학년")  # grade N
        sheet.merge_cells(start_row=2, start_column=start, end_row=2, end_column=column - 1)
    total_column = column
    sheet.cell(row=2, column=total_column, value="계")  # total
    sheet.merge_cells(start_row=2, start_column=total_column, end_row=3, end_column=total_column)

    for row in (2, 3):
        for index in range(1, total_column + 1):
            style_header(sheet.cell(row=row, column=index))

    class_columns = {
        (grade, number): len(fixed) + 1 + GRADES.index(grade) * CLASSES_PER_GRADE + (number - 1)
        for grade, number in _all_classes()
    }
    for order, (official, short, teacher, hours) in enumerate(build_rows(), start=1):
        row = 3 + order
        sheet.cell(row=row, column=1, value=order)
        sheet.cell(row=row, column=2, value=official)
        sheet.cell(row=row, column=3, value=short)
        sheet.cell(row=row, column=4, value=teacher)
        for key, value in hours.items():
            sheet.cell(row=row, column=class_columns[key], value=value).alignment = center
        sheet.cell(row=row, column=total_column, value=sum(hours.values())).alignment = center
        for index in range(1, total_column + 1):
            sheet.cell(row=row, column=index).border = box
    for index in range(1, total_column + 1):
        sheet.column_dimensions[get_column_letter(index)].width = 12 if index in (2, 3, 4) else 5

    # ---------------------------------------------------------- subjects (과목)
    subjects = workbook.create_sheet("과목")
    # short subject, official subject, special room, co-teaching, preferred periods, avoided periods
    headers = ["단축과목명", "정식과목명", "특별실", "복수교사", "선호교시", "기피교시"]
    for index, label in enumerate(headers, start=1):
        style_header(subjects.cell(row=1, column=index, value=label))
        subjects.column_dimensions[get_column_letter(index)].width = 14
    for offset, (short, full, room, preferred, avoided) in enumerate(SUBJECTS, start=2):
        subjects.cell(row=offset, column=1, value=short)
        subjects.cell(row=offset, column=2, value=full)
        subjects.cell(row=offset, column=3, value=room)
        subjects.cell(row=offset, column=4, value="")
        subjects.cell(row=offset, column=5, value=preferred)
        subjects.cell(row=offset, column=6, value=avoided)

    # ---------------------------------------------------------- special rooms (특별실)
    rooms_sheet = workbook.create_sheet("특별실")
    for index, label in enumerate(["특별실명", "종류"], start=1):  # room name, type
        style_header(rooms_sheet.cell(row=1, column=index, value=label))
        rooms_sheet.column_dimensions[get_column_letter(index)].width = 16
    for offset, (name, room_type) in enumerate(ROOMS, start=2):
        rooms_sheet.cell(row=offset, column=1, value=name)
        rooms_sheet.cell(row=offset, column=2, value=room_type)

    # ---------------------------------------------------------- special operations (특별작업)
    special = workbook.create_sheet("특별작업")
    for index, label in enumerate(["구분", "대상", "값"], start=1):  # kind, target, value
        style_header(special.cell(row=1, column=index, value=label))
        special.column_dimensions[get_column_letter(index)].width = 18
    for offset, (kind, target, value) in enumerate(SPECIAL_OPERATIONS, start=2):
        special.cell(row=offset, column=1, value=kind)
        special.cell(row=offset, column=2, value=target)
        special.cell(row=offset, column=3, value=value)
    special.cell(row=len(SPECIAL_OPERATIONS) + 3, column=1, value="How to use the kinds:")
    for offset, hint in enumerate(
        (
            "배정금지 | teacher name | 월1        - the teacher cannot teach then (월1 = Monday period 1)",
            "연속수업 | short subject | 2       - joins the subject into 2-period blocks",
            "고정배정 | 1학년 1반/Ethics | 수3 - pins that course (grade 1 class 1) to Wednesday period 3",
            "분반    | 1학년 1반/Math | MathA - courses with the same value run together",
        ),
        start=len(SPECIAL_OPERATIONS) + 4,
    ):
        special.cell(row=offset, column=1, value=hint)

    buffer = BytesIO()
    workbook.save(buffer)
    data = buffer.getvalue()
    if destination is not None:
        if isinstance(destination, str):
            with open(destination, "wb") as handle:
                handle.write(data)
        else:
            destination.write(data)
    return data


__all__ = ["CURRICULUM", "DAILY_PERIODS", "build_rows", "write_workbook"]
