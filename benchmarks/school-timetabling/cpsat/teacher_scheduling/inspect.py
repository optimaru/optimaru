"""배정검색 — 컴시간 [배정검색 및 조정] 화면에 대응하는 편성 품질 리포트.

컴시간은 검색조건마다 "문제가 있는 교사 수" 를 세어 보여주고, 항목을 누르면
해당 교사/요일로 이동한다. 여기서는 같은 검색조건들을 계산해 조건별로

    (조건 id, 이름, 문제 건수, 대상 목록)

을 돌려준다. `validate.validate_timetable` 이 하드 위반(있으면 안 되는 것)을
보는 것과 달리, 이쪽은 "편성은 됐지만 손보면 좋은 것" 을 본다.

컴시간의 [문제점만 검색] 처럼 어쩔 수 없는 항목을 숨기는 필터는 두지 않고,
호출자가 `severity` 로 판단하도록 한다.
"""

from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Literal

from pydantic import BaseModel, Field

from teacher_scheduling.domain.enums import Weekday
from teacher_scheduling.domain.problem import TimetableProblem
from teacher_scheduling.domain.solution import Timetable
from teacher_scheduling.partial import unassigned_courses

Severity = Literal["error", "warning", "info"]


class InspectOptions(BaseModel):
    """검색 기준. 컴시간의 [이동 및 검색기준] 패널에 해당한다."""

    lunch_after_period: int | None = Field(default=4, ge=1, description="점심시간 직전 교시. None 이면 점심 검사 생략")
    max_periods_per_day: int = Field(default=5, ge=1, description="이 시수를 넘게 배정된 교사를 찾는다")
    max_subjects_per_day: int = Field(default=3, ge=1, description="교사가 하루에 맡는 서로 다른 과목 수 상한")
    long_run: int = Field(default=4, ge=2, description="'연속 N시간' 으로 셀 기준")
    medium_run: int = Field(default=3, ge=2, description="'연속 N시간' 2차 기준")
    workload_spread: int = Field(default=3, ge=1, description="교사의 요일별 시수 최대-최소 허용 폭")


class Finding(BaseModel):
    """검색조건 하나에 걸린 대상 한 건."""

    subject_type: Literal["teacher", "class_group", "course"]
    subject_id: str
    subject_name: str
    day: int | None = None
    detail: str = ""


class InspectionItem(BaseModel):
    """검색조건 한 줄."""

    id: str
    name: str
    severity: Severity
    count: int
    findings: tuple[Finding, ...] = ()


class InspectionReport(BaseModel):
    """배정검색 결과 전체."""

    items: tuple[InspectionItem, ...] = ()
    hard_violations: tuple[str, ...] = ()
    total_lessons: int = 0
    required_lessons: int = 0

    @property
    def is_clean(self) -> bool:
        return not self.hard_violations and all(item.severity != "error" or item.count == 0 for item in self.items)


# ---------------------------------------------------------------- 집계 헬퍼


@dataclass
class _Index:
    """리포트 계산에 쓰는 사전 집계."""

    problem: TimetableProblem
    timetable: Timetable
    # (교사, 요일) -> 교시 집합
    teacher_periods: dict[tuple[str, Weekday], set[int]] = field(default_factory=lambda: defaultdict(set))
    # (학급, 요일) -> 교시 집합
    class_periods: dict[tuple[str, Weekday], set[int]] = field(default_factory=lambda: defaultdict(set))
    # (교사, 요일) -> 과목 집합
    teacher_subjects: dict[tuple[str, Weekday], set[str]] = field(default_factory=lambda: defaultdict(set))
    # (학급, 요일, 과목) -> 횟수
    class_subject_day: Counter = field(default_factory=Counter)

    def __post_init__(self) -> None:
        for lesson in self.timetable.lessons:
            day = lesson.slot.day
            self.class_periods[lesson.class_group_id, day].add(lesson.slot.period)
            self.class_subject_day[lesson.class_group_id, day, lesson.subject_id] += 1
            for teacher_id in lesson.teacher_ids:
                self.teacher_periods[teacher_id, day].add(lesson.slot.period)
                self.teacher_subjects[teacher_id, day].add(lesson.subject_id)

    def teacher_name(self, teacher_id: str) -> str:
        teacher = self.problem.teacher_by_id.get(teacher_id)
        return teacher.name if teacher else teacher_id

    def class_name(self, class_id: str) -> str:
        group = self.problem.class_group_by_id.get(class_id)
        return group.name if group else class_id

    def available_periods(self, class_id: str, day: Weekday) -> list[int]:
        """그 학급이 그 요일에 실제로 수업을 받는 교시들 (학년별 요일 시수 반영)."""
        blocked = {slot.period for slot in self.problem.class_group_by_id[class_id].unavailable if slot.day == day}
        total = self.problem.calendar.periods_per_day.get(day, 0)
        return [period for period in range(1, total + 1) if period not in blocked]


def _runs(periods: set[int]) -> list[int]:
    """연속 구간 길이 목록."""
    lengths: list[int] = []
    current = 0
    previous: int | None = None
    for period in sorted(periods):
        current = current + 1 if previous == period - 1 else 1
        if current == 1:
            lengths.append(1)
        else:
            lengths[-1] = current
        previous = period
    return lengths


def _gap_count(periods: set[int]) -> int:
    if not periods:
        return 0
    return max(periods) - min(periods) + 1 - len(periods)


# ---------------------------------------------------------------- 검색조건


def _unassigned(index: _Index, _: InspectOptions) -> InspectionItem:
    rows = unassigned_courses(index.problem, index.timetable)
    findings = tuple(
        Finding(
            subject_type="course",
            subject_id=row.course_id,
            subject_name=f"{row.class_group_name} {row.subject_id}",
            detail=f"{row.required}시간 중 {row.missing}시간 미배정 ({', '.join(row.teacher_names)})",
        )
        for row in rows
    )
    return InspectionItem(id="unassigned", name="미배정 시수", severity="error", count=len(findings), findings=findings)


def _same_subject_twice_a_day(index: _Index, _: InspectOptions) -> InspectionItem:
    """컴시간 [요일별 동일과목] — 한 학급이 같은 날 같은 과목을 두 번 이상."""
    findings = []
    for (class_id, day, subject_id), count in sorted(index.class_subject_day.items()):
        if count < 2:
            continue
        # 연강(block)이나 하루 2시간이 정상인 강좌는 제외한다.
        limit = max(
            (
                course.effective_max_periods_per_day
                for course in index.problem.courses_by_class_group.get(class_id, ())
                if course.subject_id == subject_id
            ),
            default=1,
        )
        if count <= limit:
            continue
        findings.append(
            Finding(
                subject_type="class_group",
                subject_id=class_id,
                subject_name=index.class_name(class_id),
                day=int(day),
                detail=f"{Weekday(day).label}요일 {subject_id} {count}시간 (허용 {limit})",
            )
        )
    return InspectionItem(
        id="same_subject_twice_a_day",
        name="요일별 동일과목 중복",
        severity="warning",
        count=len(findings),
        findings=tuple(findings),
    )


def _teacher_daily_overload(index: _Index, options: InspectOptions) -> InspectionItem:
    findings = [
        Finding(
            subject_type="teacher",
            subject_id=teacher_id,
            subject_name=index.teacher_name(teacher_id),
            day=int(day),
            detail=f"{Weekday(day).label}요일 {len(periods)}시간",
        )
        for (teacher_id, day), periods in sorted(index.teacher_periods.items())
        if len(periods) > options.max_periods_per_day
    ]
    return InspectionItem(
        id="teacher_daily_overload",
        name=f"{options.max_periods_per_day}시간 초과 배정 교사",
        severity="warning",
        count=len(findings),
        findings=tuple(findings),
    )


def _teacher_many_subjects(index: _Index, options: InspectOptions) -> InspectionItem:
    findings = [
        Finding(
            subject_type="teacher",
            subject_id=teacher_id,
            subject_name=index.teacher_name(teacher_id),
            day=int(day),
            detail=f"{Weekday(day).label}요일 {len(subjects)}과목 ({', '.join(sorted(subjects))})",
        )
        for (teacher_id, day), subjects in sorted(index.teacher_subjects.items())
        if len(subjects) >= options.max_subjects_per_day
    ]
    return InspectionItem(
        id="teacher_many_subjects",
        name=f"같은 요일 {options.max_subjects_per_day}과목 이상",
        severity="info",
        count=len(findings),
        findings=tuple(findings),
    )


def _consecutive(index: _Index, options: InspectOptions, length: int, item_id: str) -> InspectionItem:
    findings = []
    for (teacher_id, day), periods in sorted(index.teacher_periods.items()):
        runs = [run for run in _runs(periods) if run >= length]
        if not runs:
            continue
        findings.append(
            Finding(
                subject_type="teacher",
                subject_id=teacher_id,
                subject_name=index.teacher_name(teacher_id),
                day=int(day),
                detail=f"{Weekday(day).label}요일 연속 {max(runs)}시간 ({len(runs)}회)",
            )
        )
    return InspectionItem(
        id=item_id,
        name=f"연속 {length}시간 이상 배정",
        severity="warning" if length >= options.long_run else "info",
        count=len(findings),
        findings=tuple(findings),
    )


def _lunch_adjacent(index: _Index, options: InspectOptions) -> InspectionItem:
    """점심시간 전후로 이어지는 연속 수업 (컴시간 [점심시간 전후 연속배정])."""
    if options.lunch_after_period is None:
        return InspectionItem(id="lunch_adjacent", name="점심 전후 연속배정", severity="info", count=0)
    before, after = options.lunch_after_period, options.lunch_after_period + 1
    findings = [
        Finding(
            subject_type="teacher",
            subject_id=teacher_id,
            subject_name=index.teacher_name(teacher_id),
            day=int(day),
            detail=f"{Weekday(day).label}요일 {before}-{after}교시 연속",
        )
        for (teacher_id, day), periods in sorted(index.teacher_periods.items())
        if before in periods and after in periods
    ]
    return InspectionItem(
        id="lunch_adjacent",
        name="점심 전후 연속배정",
        severity="info",
        count=len(findings),
        findings=tuple(findings),
    )


def _teacher_gaps(index: _Index, _: InspectOptions) -> InspectionItem:
    findings = [
        Finding(
            subject_type="teacher",
            subject_id=teacher_id,
            subject_name=index.teacher_name(teacher_id),
            day=int(day),
            detail=f"{Weekday(day).label}요일 공강 {gaps}칸",
        )
        for (teacher_id, day), periods in sorted(index.teacher_periods.items())
        if (gaps := _gap_count(periods))
    ]
    return InspectionItem(
        id="teacher_gap", name="교사 중간 공강", severity="info", count=len(findings), findings=tuple(findings)
    )


def _class_gaps(index: _Index, _: InspectOptions) -> InspectionItem:
    """학급 시간표에 빈 칸이 생긴 경우. 학년별 요일 시수를 반영해 센다."""
    findings = []
    for group in index.problem.class_groups:
        for day in index.problem.calendar.days:
            expected = index.available_periods(group.id, day)
            if not expected:
                continue
            actual = index.class_periods.get((group.id, day), set())
            missing = sorted(set(expected) - actual)
            if missing:
                findings.append(
                    Finding(
                        subject_type="class_group",
                        subject_id=group.id,
                        subject_name=group.name,
                        day=int(day),
                        detail=f"{Weekday(day).label}요일 {', '.join(f'{p}교시' for p in missing)} 비어 있음",
                    )
                )
    return InspectionItem(
        id="class_empty_period",
        name="학급 빈 교시",
        severity="error",
        count=len(findings),
        findings=tuple(findings),
    )


def _workload_spread(index: _Index, options: InspectOptions) -> InspectionItem:
    """컴시간 [교사별 시수적작화] — 요일별 시수가 들쭉날쭉한 교사."""
    findings = []
    for teacher in index.problem.teachers:
        per_day = [len(index.teacher_periods.get((teacher.id, day), set())) for day in index.problem.calendar.days]
        if not any(per_day):
            continue
        spread = max(per_day) - min(per_day)
        if spread > options.workload_spread:
            findings.append(
                Finding(
                    subject_type="teacher",
                    subject_id=teacher.id,
                    subject_name=teacher.name,
                    detail=f"요일별 {per_day} (최대-최소 {spread})",
                )
            )
    return InspectionItem(
        id="workload_spread",
        name="교사 요일별 시수 편차",
        severity="info",
        count=len(findings),
        findings=tuple(findings),
    )


def _free_day_broken(index: _Index, _: InspectOptions) -> InspectionItem:
    findings = [
        Finding(
            subject_type="teacher",
            subject_id=teacher.id,
            subject_name=teacher.name,
            day=int(day),
            detail=f"{Weekday(day).label}요일 희망 공강인데 {len(index.teacher_periods[teacher.id, day])}시간",
        )
        for teacher in index.problem.teachers
        for day in sorted(teacher.preferred_free_days)
        if index.teacher_periods.get((teacher.id, day))
    ]
    return InspectionItem(
        id="free_day_broken",
        name="희망 공강일 위반",
        severity="info",
        count=len(findings),
        findings=tuple(findings),
    )


def inspect_timetable(
    problem: TimetableProblem,
    timetable: Timetable,
    options: InspectOptions | None = None,
) -> InspectionReport:
    """편성 결과를 검색조건별로 훑어 리포트를 만든다."""
    from teacher_scheduling.validate import validate_timetable

    options = options or InspectOptions()
    index = _Index(problem=problem, timetable=timetable)

    items = [
        _unassigned(index, options),
        _class_gaps(index, options),
        _same_subject_twice_a_day(index, options),
        _teacher_daily_overload(index, options),
        _consecutive(index, options, options.long_run, "consecutive_long"),
        _consecutive(index, options, options.medium_run, "consecutive_medium"),
        _lunch_adjacent(index, options),
        _teacher_many_subjects(index, options),
        _teacher_gaps(index, options),
        _workload_spread(index, options),
        _free_day_broken(index, options),
    ]

    # 미배정을 허용한 부분해에서는 H1 위반이 당연히 뜨므로 그 메시지는 걸러낸다
    # (같은 내용을 `unassigned` 항목이 이미 구조화해서 보여준다).
    violations = tuple(v for v in validate_timetable(problem, timetable) if "주당 시수" not in v)

    return InspectionReport(
        items=tuple(items),
        hard_violations=violations,
        total_lessons=len(timetable.lessons),
        required_lessons=sum(course.periods_per_week for course in problem.courses),
    )


__all__ = [
    "Finding",
    "InspectOptions",
    "InspectionItem",
    "InspectionReport",
    "inspect_timetable",
]
