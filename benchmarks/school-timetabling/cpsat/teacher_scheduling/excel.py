"""컴시간(comcigan) 형식 교사별 시수표 엑셀 -> TimetableProblem.

컴시간알리미의 [기초자료]-[교사별시수표 불러오기] 가 받는 엑셀 양식을 그대로
읽는다. 학교가 이미 쓰고 있는 파일을 그대로 올릴 수 있게 하는 것이 목적이라
필수 시트는 컴시간과 같은 `교사별시수표` 하나이고, 컴시간이 별도 화면에서
입력받는 것들(요일별 시수/기본사항/과목등록부/특별실/특별작업)은 선택 시트로
받는다.

시트 구성
---------
- `기본사항`   (필수) 학교명/학년도/학년별 학급수
- `요일별시수` (필수) 학년 x 요일 -> 교시 수
- `교사별시수표` (필수) 컴시간 원본 양식
- `과목`       (선택) 단축과목명별 특별실/복수교사/선호·기피 교시
- `특별실`     (선택) 특별실명 + 종류
- `특별작업`   (선택) 연속수업/배정금지/고정배정/분반

학년별 요일별 시수
------------------
도메인의 `SchoolCalendar` 는 전교 공통이라 학년마다 다른 요일별 시수를 직접
담지 못한다. 그래서 캘린더에는 요일별 **최대** 교시를 넣고, 그보다 짧은 학년의
남는 칸은 그 학년 학급들의 `ClassGroup.unavailable` 로 막는다 — 솔버 입장에서는
"그 학급은 그 시간에 수업이 없다"와 같은 뜻이고, 계약(API_SPEC)도 그대로 둔다.
"""

from __future__ import annotations

import re
from collections import defaultdict
from dataclasses import dataclass, field
from typing import IO, Any

from teacher_scheduling.domain.entities import ClassGroup, Course, Room, Subject, Teacher, TimeSlot
from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.domain.problem import SchoolCalendar, TimetableProblem

SHEET_BASICS = "기본사항"
SHEET_DAILY = "요일별시수"
SHEET_LOAD = "교사별시수표"
SHEET_SUBJECTS = "과목"
SHEET_ROOMS = "특별실"
SHEET_SPECIAL = "특별작업"

_DAY_BY_LABEL = {day.label: day for day in Weekday}
_GRADE_RE = re.compile(r"(\d+)\s*학년")
# "월1", "월 1교시", "월요일 1교시" 를 모두 받는다.
_SLOT_RE = re.compile(r"^\s*([월화수목금토])\s*(?:요일)?\s*(\d+)\s*(?:교시)?\s*$")


class ExcelFormatError(ValueError):
    """엑셀 양식이 계약과 맞지 않을 때. 사용자에게 그대로 보여줄 메시지를 담는다."""


@dataclass
class SchoolBasics:
    """컴시간 [기본사항 등록] 에 대응하는 학교 메타데이터."""

    school_name: str = ""
    school_year: int | None = None
    classes_per_grade: dict[int, int] = field(default_factory=dict)


@dataclass
class ImportResult:
    """임포트 결과. `problem` 이 본체이고 나머지는 화면에 그대로 보여주는 부가정보."""

    problem: TimetableProblem
    basics: SchoolBasics
    # 단축과목명 -> 정식과목명들 (NEIS 내보내기에서 쓴다)
    official_subject_names: dict[str, list[str]] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


# ---------------------------------------------------------------- 셀 유틸


def _norm(value: Any) -> str:
    """셀 값을 비교 가능한 문자열로. None/공백/숫자를 모두 흡수한다."""
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
        raise ExcelFormatError(f"시간 표기를 읽을 수 없습니다: {text!r} (예: '월1' 또는 '월 1교시')")
    day_label, period = match.groups()
    return TimeSlot(day=_DAY_BY_LABEL[day_label], period=int(period))


def _parse_periods(text: str) -> frozenset[int]:
    """'1,2,3' 또는 '1-4' 형태의 교시 목록."""
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


# ---------------------------------------------------------------- 시트 파서


def _read_basics(rows: list[list[Any]]) -> SchoolBasics:
    """`항목 | 값` 2열 형식. 학급수는 'N학년 학급수' 로 적는다."""
    basics = SchoolBasics()
    for row in rows:
        if not row:
            continue
        key, value = _norm(row[0]), (row[1] if len(row) > 1 else None)
        if not key:
            continue
        if key == "학교명":
            basics.school_name = _norm(value)
        elif key in ("학년도", "학년"):
            basics.school_year = _as_int(value)
        elif (match := _GRADE_RE.search(key)) and "학급수" in key:
            count = _as_int(value)
            if count:
                basics.classes_per_grade[int(match.group(1))] = count
    if not basics.classes_per_grade:
        raise ExcelFormatError(f"[{SHEET_BASICS}] 시트에서 'N학년 학급수' 를 찾지 못했습니다")
    return basics


def _read_daily_periods(rows: list[list[Any]]) -> dict[int, dict[Weekday, int]]:
    """`요일별시수` 시트. 첫 행이 요일 헤더, 이후 행이 학년별 교시 수."""
    header: list[Weekday | None] = []
    per_grade: dict[int, dict[Weekday, int]] = {}
    for row in rows:
        if not row:
            continue
        label = _norm(row[0])
        if not header:
            # 헤더 행: 월~토 라벨이 있는 열 위치를 기억한다.
            # Weekday.MON 은 0 이라 falsy 다 — `is not None` 으로 봐야 월요일만
            # 있는 시트도 헤더로 인식된다.
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
        raise ExcelFormatError(f"[{SHEET_DAILY}] 시트에서 학년별 요일 시수를 찾지 못했습니다")
    return per_grade


def _read_subject_meta(rows: list[list[Any]]) -> dict[str, dict[str, Any]]:
    """`과목` 시트 (선택). 헤더 이름으로 열을 찾는다."""
    if not rows:
        return {}
    header = {_norm(cell): index for index, cell in enumerate(rows[0])}
    if "단축과목명" not in header:
        return {}

    def cell(row: list[Any], name: str) -> str:
        index = header.get(name)
        return _norm(row[index]) if index is not None and index < len(row) else ""

    meta: dict[str, dict[str, Any]] = {}
    for row in rows[1:]:
        short = cell(row, "단축과목명")
        if not short:
            continue
        meta[short] = {
            "required_room_type": cell(row, "특별실") or None,
            "team_teaching": cell(row, "복수교사") in ("O", "o", "복수", "Y", "y", "TRUE", "True", "1"),
            "preferred_periods": _parse_periods(cell(row, "선호교시")),
            "avoided_periods": _parse_periods(cell(row, "기피교시")),
        }
    return meta


def _read_rooms(rows: list[list[Any]]) -> list[Room]:
    """`특별실` 시트 (선택). `특별실명 | 종류`."""
    rooms: list[Room] = []
    for index, row in enumerate(rows):
        if not row:
            continue
        name, room_type = _norm(row[0]), _norm(row[1]) if len(row) > 1 else ""
        if not name or name in ("특별실명", "실명"):
            continue
        if not room_type:
            raise ExcelFormatError(f"[{SHEET_ROOMS}] {name}: 종류가 비어 있습니다")
        rooms.append(Room(id=f"R{index:02d}", name=name, room_type=room_type))
    return rooms


@dataclass
class _LoadRow:
    """`교사별시수표` 한 줄: 한 (정식과목, 단축과목, 교사) 의 학급별 시수."""

    official: str
    short: str
    teacher: str
    hours: dict[tuple[int, int], int]  # (학년, 반) -> 시수


def _read_load_table(rows: list[list[Any]]) -> tuple[list[_LoadRow], dict[int, int]]:
    """컴시간 원본 양식을 읽어 행 목록과 학년별 최대 반 번호를 돌려준다.

    헤더는 2행 구조다 — 위 행에 '1학년/2학년/3학년'(병합), 아래 행에 반 번호.
    병합 셀은 openpyxl 이 첫 칸에만 값을 주므로 왼쪽 값을 이어서 채운다.
    """
    header_index = next(
        (index for index, row in enumerate(rows) if any(_norm(cell) == "정식과목명" for cell in row)),
        None,
    )
    if header_index is None:
        raise ExcelFormatError(f"[{SHEET_LOAD}] 시트에서 '정식과목명' 헤더를 찾지 못했습니다")

    header = rows[header_index]
    columns = {_norm(cell): index for index, cell in enumerate(header)}
    for required in ("정식과목명", "단축과목명", "교사명"):
        if required not in columns:
            raise ExcelFormatError(f"[{SHEET_LOAD}] 헤더에 '{required}' 열이 없습니다")

    # 학년 라벨을 오른쪽으로 흘려보내고(병합 보정), 아래 행의 반 번호와 짝짓는다.
    grade_of_column: dict[int, int] = {}
    current: int | None = None
    for index, cell in enumerate(header):
        if match := _GRADE_RE.search(_norm(cell)):
            current = int(match.group(1))
        if current is not None and index > columns["교사명"]:
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
        raise ExcelFormatError(f"[{SHEET_LOAD}] 학년/반 헤더를 읽지 못했습니다 (학년 행 아래에 반 번호가 필요합니다)")

    order_column = columns.get("순")
    entries: list[_LoadRow] = []
    for row in rows[header_index + 2 :]:
        if not row:
            continue
        # 컴시간 규칙: [순] 번호가 없으면 자료의 끝.
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
        raise ExcelFormatError(f"[{SHEET_LOAD}] 시수 데이터가 한 줄도 없습니다")
    return entries, dict(max_class)


#: `특별작업` 시트에서 받는 구분. 이 목록 밖의 값은 표 끝으로 본다.
SPECIAL_KINDS = frozenset({"배정금지", "이동금지", "특별교사", "연속수업", "고정배정", "분반", "동시수업"})


def _read_special(rows: list[list[Any]]) -> list[tuple[str, str, str]]:
    """`특별작업` 시트 (선택). `구분 | 대상 | 값` 3열.

    빈 줄을 만나면 거기서 끝낸다 — 표 아래에 사용법 안내를 적어 두는 양식이라
    안내문이 데이터로 섞이면 안 된다.
    """
    result = []
    for row in rows:
        kind = _norm(row[0]) if row else ""
        if not kind:
            if result:
                break
            continue
        if kind == "구분":
            continue
        if kind not in SPECIAL_KINDS:
            break
        target = _norm(row[1]) if len(row) > 1 else ""
        value = _norm(row[2]) if len(row) > 2 else ""
        result.append((kind, target, value))
    return result


# ---------------------------------------------------------------- 조립


def _class_id(grade: int, number: int) -> str:
    return f"C{grade}{number:02d}"


def _build_class_groups(
    basics: SchoolBasics,
    daily: dict[int, dict[Weekday, int]],
    calendar: SchoolCalendar,
) -> list[ClassGroup]:
    """학년별 요일 시수가 전교 최대보다 짧으면 남는 칸을 학급 불가 시간으로 막는다."""
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
    """`특별작업` 시트를 도메인 필드로 옮긴다."""
    for kind, target, value in operations:
        if kind in ("배정금지", "이동금지", "특별교사"):
            teacher_id = teacher_id_by_name.get(target)
            if teacher_id is None:
                warnings.append(f"[{SHEET_SPECIAL}] 배정금지: 교사 '{target}' 을 시수표에서 찾지 못했습니다")
                continue
            unavailable[teacher_id].add(_parse_slot(value))
        elif kind == "연속수업":
            if target not in subject_ids:
                warnings.append(f"[{SHEET_SPECIAL}] 연속수업: 과목 '{target}' 이 없습니다")
                continue
            size = _as_int(value) or 2
            for (_, subject_id), course_ids in course_index.items():
                if subject_id == target:
                    for course_id in course_ids:
                        block_periods[course_id] = size
        elif kind == "고정배정":
            class_name, _, subject_name = target.partition("/")
            course_ids = course_index.get((class_name.strip(), subject_name.strip()), [])
            if not course_ids:
                warnings.append(f"[{SHEET_SPECIAL}] 고정배정: 강좌 '{target}' 을 찾지 못했습니다")
                continue
            for course_id in course_ids:
                fixed_slots[course_id].add(_parse_slot(value))
        elif kind in ("분반", "동시수업"):
            class_name, _, subject_name = target.partition("/")
            course_ids = course_index.get((class_name.strip(), subject_name.strip()), [])
            if not course_ids:
                warnings.append(f"[{SHEET_SPECIAL}] 분반: 강좌 '{target}' 을 찾지 못했습니다")
                continue
            for course_id in course_ids:
                parallel[course_id] = value or target
        else:
            warnings.append(f"[{SHEET_SPECIAL}] 알 수 없는 구분: {kind}")


def build_problem(
    *,
    basics: SchoolBasics,
    daily: dict[int, dict[Weekday, int]],
    load_rows: list[_LoadRow],
    subject_meta: dict[str, dict[str, Any]],
    rooms: list[Room],
    special: list[tuple[str, str, str]],
) -> ImportResult:
    """파싱된 시트들을 하나의 `TimetableProblem` 으로 조립한다."""
    warnings: list[str] = []

    # 캘린더: 요일별로 학년 중 가장 긴 교시 수.
    calendar_days: dict[Weekday, int] = defaultdict(int)
    for grade_days in daily.values():
        for day, periods in grade_days.items():
            calendar_days[day] = max(calendar_days[day], periods)
    calendar = SchoolCalendar(periods_per_day=dict(calendar_days))

    class_groups = _build_class_groups(basics, daily, calendar)
    class_ids = {group.id for group in class_groups}
    class_id_by_name = {group.name: group.id for group in class_groups}

    # 과목: 단축과목명이 편성 단위다 (정식과목명 여러 개가 한 단축명에 붙는다).
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
                f"[{SHEET_SUBJECTS}] '{subject.id}' 이 요구하는 특별실 '{subject.required_room_type}' 이"
                f" [{SHEET_ROOMS}] 시트에 없습니다"
            )

    # 교사: 이름 등장 순서로 T01.. 부여.
    teacher_names = list(dict.fromkeys(row.teacher for row in load_rows))
    teacher_id_by_name = {name: f"T{index:02d}" for index, name in enumerate(teacher_names, start=1)}
    teaches: dict[str, set[str]] = defaultdict(set)
    for row in load_rows:
        teaches[teacher_id_by_name[row.teacher]].add(row.short)

    # 강좌: (학급, 단축과목, 교사) 단위로 시수를 합친다.
    #   같은 셀에 여러 정식과목이 흩어져 있어도(수학7-가/수학7-나) 한 강좌가 된다.
    merged: dict[tuple[str, str, str], int] = defaultdict(int)
    for row in load_rows:
        teacher_id = teacher_id_by_name[row.teacher]
        for (grade, number), hours in row.hours.items():
            class_id = _class_id(grade, number)
            if class_id not in class_ids:
                warnings.append(
                    f"[{SHEET_LOAD}] {row.short}/{row.teacher}: {grade}학년 {number}반은"
                    f" [{SHEET_BASICS}] 의 학급수를 벗어나 무시합니다"
                )
                continue
            merged[class_id, row.short, teacher_id] += hours

    # 복수교사 과목은 같은 (학급, 과목) 의 교사들을 한 강좌로 합친다.
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
                f"[{SHEET_LOAD}] 복수교사 과목 {short}/{class_id} 의 교사별 시수가 달라 최소값으로 맞춥니다"
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

    # `특별작업` 은 학급/과목을 사람이 읽는 이름으로 가리키므로 이름 색인도 만든다.
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
                # 주 5일에 담기려면 하루 상한이 최소 ceil(시수/5) 는 돼야 한다.
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
    """컴시간의 '요일별시수 계 == 시수표 계' 확인에 해당하는 사전 경고."""
    for group in problem.class_groups:
        available = sum(
            1
            for day, periods in problem.calendar.periods_per_day.items()
            for period in range(1, periods + 1)
            if TimeSlot(day=day, period=period) not in group.unavailable
        )
        assigned = sum(course.periods_per_week for course in problem.courses_by_class_group.get(group.id, ()))
        if assigned > available:
            warnings.append(f"{group.name}: 시수 합 {assigned}시간 > 배정 가능 {available}칸 — 편성 불가")
        elif assigned < available:
            warnings.append(
                f"{group.name}: 시수 합 {assigned}시간 < 배정 가능 {available}칸 — {available - assigned}칸이 빕니다"
            )


# ---------------------------------------------------------------- 진입점


def import_workbook(source: str | IO[bytes]) -> ImportResult:
    """엑셀 파일(경로 또는 바이너리 스트림)을 읽어 문제로 만든다."""
    try:
        from openpyxl import load_workbook
    except ImportError as exc:  # pragma: no cover - 의존성 누락 환경
        raise ExcelFormatError("openpyxl 이 설치되어 있지 않습니다") from exc

    # .xlsx 는 zip 이라 엑셀이 아닌 파일은 BadZipFile 로 터진다. 사용자가 잘못
    # 올린 것뿐이므로 양식 오류로 바꿔 준다.
    try:
        workbook = load_workbook(source, data_only=True, read_only=True)
    except ExcelFormatError:
        raise
    except Exception as exc:
        raise ExcelFormatError(f"엑셀 파일(.xlsx)로 읽을 수 없습니다: {exc}") from exc

    try:
        available = set(workbook.sheetnames)
        for required in (SHEET_BASICS, SHEET_DAILY, SHEET_LOAD):
            if required not in available:
                raise ExcelFormatError(f"필수 시트 '{required}' 가 없습니다 (있는 시트: {sorted(available)})")

        basics = _read_basics(_rows(workbook[SHEET_BASICS]))
        daily = _read_daily_periods(_rows(workbook[SHEET_DAILY]))
        load_rows, max_class = _read_load_table(_rows(workbook[SHEET_LOAD]))
        subject_meta = _read_subject_meta(_rows(workbook[SHEET_SUBJECTS])) if SHEET_SUBJECTS in available else {}
        rooms = _read_rooms(_rows(workbook[SHEET_ROOMS])) if SHEET_ROOMS in available else []
        special = _read_special(_rows(workbook[SHEET_SPECIAL])) if SHEET_SPECIAL in available else []
    finally:
        workbook.close()

    # 기본사항의 학급수가 비어 있으면 시수표의 반 번호에서 유추한다.
    for grade, count in max_class.items():
        basics.classes_per_grade.setdefault(grade, count)

    missing = sorted(set(max_class) - set(daily))
    if missing:
        raise ExcelFormatError(f"[{SHEET_DAILY}] 시트에 {missing} 학년의 요일별 시수가 없습니다")

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
