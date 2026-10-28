"""컴시간 엑셀 임포트 / 배정검색 / 내보내기."""

import csv
import io
import zipfile

import pytest
from openpyxl import Workbook, load_workbook

from teacher_scheduling import (
    ExcelFormatError,
    ExportOptions,
    NeisFormat,
    SolverConfig,
    import_workbook,
    inspect_timetable,
    neis_bundle,
    neis_zip,
    solve_partial,
    timetable_workbook,
    unassigned_courses,
)
from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.excel import SHEET_BASICS, SHEET_DAILY, SHEET_LOAD, SHEET_SPECIAL
from teacher_scheduling.sample_workbook import CURRICULUM, DAILY_PERIODS, write_workbook


@pytest.fixture(scope="module")
def sample_bytes() -> bytes:
    return write_workbook()


@pytest.fixture(scope="module")
def imported(sample_bytes: bytes):
    return import_workbook(io.BytesIO(sample_bytes))


@pytest.fixture(scope="module")
def solved(imported):
    return solve_partial(imported.problem, SolverConfig(time_limit_seconds=60))


# ---------------------------------------------------------------- 임포트


def test_sample_workbook_imports_without_warnings(imported):
    assert imported.warnings == []
    assert imported.basics.school_name == "컴시간중학교"
    assert imported.basics.school_year == 2026
    assert imported.basics.classes_per_grade == {1: 3, 2: 3, 3: 3}


def test_calendar_is_the_longest_day_across_grades(imported):
    # 3학년만 월/화가 7교시이므로 전교 캘린더도 7교시가 된다.
    assert imported.problem.calendar.periods_per_day[Weekday.MON] == 7
    assert imported.problem.calendar.periods_per_day[Weekday.FRI] == 6


def test_shorter_grades_block_the_extra_periods(imported):
    """학년별 요일 시수는 학급 불가 시간으로 표현된다."""
    by_id = imported.problem.class_group_by_id
    first = by_id["C101"]
    third = by_id["C301"]
    blocked = {(slot.day, slot.period) for slot in first.unavailable}
    assert (Weekday.MON, 7) in blocked, "1학년 월요일은 6교시까지"
    assert (Weekday.TUE, 7) in blocked
    assert third.unavailable == frozenset(), "3학년은 캘린더를 꽉 채운다"


def test_weekly_hours_match_the_curriculum(imported):
    problem = imported.problem
    for group in problem.class_groups:
        assigned = sum(c.periods_per_week for c in problem.courses_by_class_group[group.id])
        assert assigned == sum(CURRICULUM[group.grade].values())
        assert assigned == sum(DAILY_PERIODS[group.grade].values())


def test_official_names_collapse_into_one_short_subject(imported):
    # 기가 = 기술·가정. 편성은 단축명 기준이고 정식명은 NEIS 용으로 남는다.
    assert "기가" in imported.problem.subject_by_id
    assert imported.official_subject_names["기가"] == ["기술·가정"]


def test_special_sheet_applies_blocks_and_bans(imported):
    problem = imported.problem
    art = [c for c in problem.courses if c.subject_id == "미술"]
    assert art and all(c.block_periods == 2 for c in art), "연속수업 2연강"

    banned = {t.name: {str(s) for s in t.unavailable} for t in problem.teachers if t.unavailable}
    assert banned == {"김하늘": {"월1"}, "권나윤": {"금6"}}


def test_usage_hints_below_the_table_are_not_parsed(imported):
    """`특별작업` 표 아래 안내문이 데이터로 섞이면 안 된다."""
    assert not any("사용법" in w for w in imported.warnings)


def test_subject_room_requirement_comes_from_subject_sheet(imported):
    assert imported.problem.subject_by_id["과학"].required_room_type == "과학실"
    assert imported.problem.subject_by_id["국어"].required_room_type is None
    assert imported.problem.subject_by_id["체육"].avoided_periods == frozenset({1})
    assert imported.problem.subject_by_id["수학"].preferred_periods == frozenset({1, 2, 3, 4})


# ---------------------------------------------------------------- 양식 오류


def _minimal_workbook(**sheets: list[list]) -> io.BytesIO:
    workbook = Workbook()
    workbook.remove(workbook.active)
    for name, rows in sheets.items():
        sheet = workbook.create_sheet(name)
        for row in rows:
            sheet.append(row)
    buffer = io.BytesIO()
    workbook.save(buffer)
    buffer.seek(0)
    return buffer


def test_missing_required_sheet_is_reported():
    with pytest.raises(ExcelFormatError, match="필수 시트"):
        import_workbook(_minimal_workbook(**{SHEET_BASICS: [["학교명", "가"]]}))


def test_missing_header_column_is_reported():
    source = _minimal_workbook(
        **{
            SHEET_BASICS: [["1학년 학급수", 1]],
            SHEET_DAILY: [["학년", "월"], ["1학년", 3]],
            SHEET_LOAD: [["순", "단축과목명", "교사명"]],
        }
    )
    with pytest.raises(ExcelFormatError, match="정식과목명"):
        import_workbook(source)


def test_capacity_mismatch_becomes_a_warning():
    """시수 합이 요일별 시수와 다르면 컴시간처럼 미리 알려준다."""
    source = _minimal_workbook(
        **{
            SHEET_BASICS: [["1학년 학급수", 1]],
            SHEET_DAILY: [["학년", "월", "화"], ["1학년", 3, 3]],
            SHEET_LOAD: [
                ["순", "정식과목명", "단축과목명", "교사명", "1학년", None],
                [None, None, None, None, 1, None],
                [1, "국어", "국어", "김교사", 4, None],
            ],
        }
    )
    result = import_workbook(source)
    assert any("2칸이 빕니다" in w for w in result.warnings)


# ---------------------------------------------------------------- 편성 + 배정검색


def test_imported_problem_solves_completely(solved, imported):
    required = sum(c.periods_per_week for c in imported.problem.courses)
    assert solved.status.has_solution
    assert len(solved.lessons) == required
    assert unassigned_courses(imported.problem, solved) == []


def test_inspection_has_no_errors_on_a_full_solve(imported, solved):
    report = inspect_timetable(imported.problem, solved)
    assert report.hard_violations == ()
    errors = {item.id: item.count for item in report.items if item.severity == "error"}
    assert errors == {"unassigned": 0, "class_empty_period": 0}
    assert report.total_lessons == report.required_lessons


def test_inspection_reports_every_search_condition(imported, solved):
    report = inspect_timetable(imported.problem, solved)
    ids = {item.id for item in report.items}
    assert {
        "unassigned",
        "class_empty_period",
        "same_subject_twice_a_day",
        "teacher_daily_overload",
        "consecutive_long",
        "lunch_adjacent",
        "teacher_gap",
    } <= ids


def test_partial_solve_leaves_unassigned_instead_of_infeasible():
    """채울 수 없는 시수는 미배정으로 남고 나머지는 배정된다."""
    source = _minimal_workbook(
        **{
            SHEET_BASICS: [["1학년 학급수", 1]],
            SHEET_DAILY: [["학년", "월"], ["1학년", 2]],
            SHEET_LOAD: [
                ["순", "정식과목명", "단축과목명", "교사명", "1학년", None],
                [None, None, None, None, 1, None],
                # 월요일 2칸뿐인데 5시간을 요구한다 -> 3시간 미배정
                [1, "국어", "국어", "김교사", 5, None],
            ],
        }
    )
    problem = import_workbook(source).problem
    timetable = solve_partial(problem, SolverConfig(time_limit_seconds=10))
    assert timetable.status.has_solution, "부분 편성은 INFEASIBLE 이 아니어야 한다"
    assert len(timetable.lessons) == 2
    missing = unassigned_courses(problem, timetable)
    assert [(u.subject_id, u.missing) for u in missing] == [("국어", 3)]

    report = inspect_timetable(problem, timetable)
    assert next(item for item in report.items if item.id == "unassigned").count == 1


def _parallel_workbook(periods_per_week: int, periods_on_monday: int) -> io.BytesIO:
    """1학년 1반의 기술/가정을 한 분반 그룹으로 묶은 최소 양식."""
    return _minimal_workbook(
        **{
            SHEET_BASICS: [["1학년 학급수", 1]],
            SHEET_DAILY: [["학년", "월"], ["1학년", periods_on_monday]],
            SHEET_LOAD: [
                ["순", "정식과목명", "단축과목명", "교사명", "1학년", None],
                [None, None, None, None, 1, None],
                [1, "기술", "기술", "김교사", periods_per_week, None],
                [2, "가정", "가정", "이교사", periods_per_week, None],
            ],
            SHEET_SPECIAL: [
                ["구분", "대상", "값"],
                ["분반", "1학년 1반/기술", "기가A"],
                ["분반", "1학년 1반/가정", "기가A"],
            ],
        }
    )


def test_parallel_group_is_not_penalized_twice_for_one_missing_period():
    """분반은 H5 로 항상 같이 움직이므로 미배정 벌점도 그룹당 한 번만 센다."""
    # 월요일 2칸뿐인데 분반 두 강좌가 각각 4시수 -> 그룹으로 2시수 미배정
    problem = import_workbook(_parallel_workbook(periods_per_week=4, periods_on_monday=2)).problem
    timetable = solve_partial(problem, SolverConfig(time_limit_seconds=10))

    assert len(timetable.lessons) == 4, "2칸을 분반 두 강좌가 나눠 쓴다"
    missing = {u.subject_id: u.missing for u in unassigned_courses(problem, timetable)}
    assert missing == {"기술": 2, "가정": 2}, "미배정 목록은 강좌마다 그대로 보여준다"
    assert timetable.penalties["weekly_period_counts"] == 2, "그룹 크기만큼 곱해지면 안 된다"


# ---------------------------------------------------------------- 내보내기


def test_neis_bundle_has_one_csv_per_class(imported, solved):
    files = neis_bundle(imported.problem, solved, ExportOptions(school_year=2026, semester=1))
    assert len(files) == len(imported.problem.class_groups)
    assert "기초시간표(2026-1,1-1).csv" in files

    rows = list(csv.reader(io.StringIO(files["기초시간표(2026-1,1-1).csv"].decode("cp949"))))
    assert rows[0] == ["교시", "월요일", "화요일", "수요일", "목요일", "금요일"]
    # 1학년 월요일은 6교시까지 -> 7교시 행의 월요일 칸은 비어 있다.
    assert rows[7][1] == ""
    assert all("/" in cell for cell in rows[1][1:] if cell)


def test_neis_uses_official_subject_names(imported, solved):
    options = ExportOptions(
        school_year=2026,
        official_subject_names={k: v[0] for k, v in imported.official_subject_names.items()},
    )
    text = neis_bundle(imported.problem, solved, options)["기초시간표(2026-1,1-1).csv"].decode("cp949")
    assert "기술·가정" in text and "기가/" not in text


def test_neis_format_can_widen_to_seven_day_columns(imported, solved):
    """NEIS 화면이 월~일 7열이라 열 개수를 늘릴 수 있어야 한다."""
    options = ExportOptions(school_year=2026, neis=NeisFormat(day_count=7))
    text = neis_bundle(imported.problem, solved, options)["기초시간표(2026-1,1-1).csv"].decode("cp949")
    rows = list(csv.reader(io.StringIO(text)))
    assert rows[0] == ["교시", "월요일", "화요일", "수요일", "목요일", "금요일", "토요일", "일요일"]
    # 수업이 없는 토/일은 빈 열로 채워진다
    assert all(row[6] == "" and row[7] == "" for row in rows[1:])


def test_neis_format_can_split_the_teacher_into_its_own_column(imported, solved):
    options = ExportOptions(school_year=2026, neis=NeisFormat(teacher_column=True))
    text = neis_bundle(imported.problem, solved, options)["기초시간표(2026-1,1-1).csv"].decode("cp949")
    rows = list(csv.reader(io.StringIO(text)))
    assert rows[0][:3] == ["교시", "월요일", "월요일 교사"]
    subject, teacher = rows[1][1], rows[1][2]
    assert subject and teacher and "/" not in subject


def test_neis_format_can_drop_the_header_and_period_suffix(imported, solved):
    options = ExportOptions(
        school_year=2026,
        neis=NeisFormat(include_header=False, period_suffix=""),
    )
    text = neis_bundle(imported.problem, solved, options)["기초시간표(2026-1,1-1).csv"].decode("cp949")
    rows = list(csv.reader(io.StringIO(text)))
    assert rows[0][0] == "1", "헤더가 없으면 첫 행이 바로 1교시"
    assert len(rows) == imported.problem.calendar.max_periods


@pytest.fixture(scope="module")
def parallel_solved():
    """분반 두 강좌가 같은 칸에 들어가는 최소 시간표."""
    problem = import_workbook(_parallel_workbook(periods_per_week=2, periods_on_monday=2)).problem
    return problem, solve_partial(problem, SolverConfig(time_limit_seconds=10))


def test_neis_writes_every_course_sharing_a_cell(parallel_solved):
    """한 칸의 분반이 조용히 하나만 남으면 안 된다."""
    problem, timetable = parallel_solved
    files = neis_bundle(problem, timetable, ExportOptions(school_year=2026))
    cell = list(csv.reader(io.StringIO(files["기초시간표(2026-1,1-1).csv"].decode("cp949"))))[1][1]
    assert "기술/김교사" in cell and "가정/이교사" in cell
    assert cell.count(";") == 1


def test_neis_can_keep_only_one_course_per_cell(parallel_solved):
    """NEIS 가 한 칸에서 과목 하나만 읽으면 대표만 남긴다."""
    problem, timetable = parallel_solved
    options = ExportOptions(school_year=2026, neis=NeisFormat(parallel_separator=None))
    text = neis_bundle(problem, timetable, options)["기초시간표(2026-1,1-1).csv"].decode("cp949")
    cell = list(csv.reader(io.StringIO(text)))[1][1]
    assert cell in {"기술/김교사", "가정/이교사"}


def test_neis_format_rejects_an_impossible_day_count():
    with pytest.raises(ValueError, match="day_count"):
        NeisFormat(day_count=8)


def test_neis_zip_contains_the_same_files(imported, solved):
    data = neis_zip(imported.problem, solved, ExportOptions(school_year=2026))
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        assert len(archive.namelist()) == len(imported.problem.class_groups)


def test_print_workbook_has_the_expected_sheets(imported, solved):
    data = timetable_workbook(imported.problem, solved, ExportOptions(school_year=2026))
    workbook = load_workbook(io.BytesIO(data))
    assert {"전체시간표", "학급시간표", "교사시간표", "특별실시간표", "교사시수"} <= set(workbook.sheetnames)
    summary = workbook["교사시수"]
    total = sum(summary.cell(row=r, column=3).value for r in range(2, summary.max_row + 1))
    assert total == sum(len(lesson.teacher_ids) for lesson in solved.lessons)
