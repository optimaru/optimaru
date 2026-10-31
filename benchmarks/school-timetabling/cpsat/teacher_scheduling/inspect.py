"""Assignment inspection — a timetable quality report matching Comcigan's "search and adjust assignments" screen.

For each search condition Comcigan shows "how many teachers have a problem",
and clicking an item jumps to that teacher/day. This computes the same search
conditions and returns, per condition,

    (condition id, name, problem count, affected items)

`validate.validate_timetable` looks at hard violations (things that must not
happen); this looks at "scheduled, but worth touching up".

There is no filter hiding unavoidable items like Comcigan's "problems only"
search; the caller decides from `severity`.
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
    """Search criteria. Matches Comcigan's "move and search criteria" panel."""

    lunch_after_period: int | None = Field(default=4, ge=1, description="Period right before lunch. None skips the lunch check")
    max_periods_per_day: int = Field(default=5, ge=1, description="Finds teachers with more periods than this in a day")
    max_subjects_per_day: int = Field(default=3, ge=1, description="Cap on distinct subjects a teacher teaches in a day")
    long_run: int = Field(default=4, ge=2, description="Threshold counted as 'N consecutive periods'")
    medium_run: int = Field(default=3, ge=2, description="Second threshold for 'N consecutive periods'")
    workload_spread: int = Field(default=3, ge=1, description="Allowed max-min spread of a teacher's periods per weekday")


class Finding(BaseModel):
    """One item caught by a search condition."""

    subject_type: Literal["teacher", "class_group", "course"]
    subject_id: str
    subject_name: str
    day: int | None = None
    detail: str = ""


class InspectionItem(BaseModel):
    """One search-condition row."""

    id: str
    name: str
    severity: Severity
    count: int
    findings: tuple[Finding, ...] = ()


class InspectionReport(BaseModel):
    """The whole inspection result."""

    items: tuple[InspectionItem, ...] = ()
    hard_violations: tuple[str, ...] = ()
    total_lessons: int = 0
    required_lessons: int = 0


# ---------------------------------------------------------------- aggregation helpers


@dataclass
class _Index:
    """Pre-aggregations used to compute the report."""

    problem: TimetableProblem
    timetable: Timetable
    # (teacher, weekday) -> set of periods
    teacher_periods: dict[tuple[str, Weekday], set[int]] = field(default_factory=lambda: defaultdict(set))
    # (class, weekday) -> set of periods
    class_periods: dict[tuple[str, Weekday], set[int]] = field(default_factory=lambda: defaultdict(set))
    # (teacher, weekday) -> set of subjects
    teacher_subjects: dict[tuple[str, Weekday], set[str]] = field(default_factory=lambda: defaultdict(set))
    # (class, weekday, subject) -> count
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
        """The periods in which the class actually has lessons that weekday (per-grade periods per weekday applied)."""
        blocked = {slot.period for slot in self.problem.class_group_by_id[class_id].unavailable if slot.day == day}
        total = self.problem.calendar.periods_per_day.get(day, 0)
        return [period for period in range(1, total + 1) if period not in blocked]


def _runs(periods: set[int]) -> list[int]:
    """Lengths of consecutive runs."""
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


# ---------------------------------------------------------------- search conditions


def _unassigned(index: _Index, _: InspectOptions) -> InspectionItem:
    rows = unassigned_courses(index.problem, index.timetable)
    findings = tuple(
        Finding(
            subject_type="course",
            subject_id=row.course_id,
            subject_name=f"{row.class_group_name} {row.subject_id}",
            detail=f"{row.missing} of {row.required} periods unassigned ({', '.join(row.teacher_names)})",
        )
        for row in rows
    )
    return InspectionItem(id="unassigned", name="Unassigned periods", severity="error", count=len(findings), findings=findings)


def _same_subject_twice_a_day(index: _Index, _: InspectOptions) -> InspectionItem:
    """Comcigan's "same subject per weekday" — a class has the same subject twice or more in one day."""
    findings = []
    for (class_id, day, subject_id), count in sorted(index.class_subject_day.items()):
        if count < 2:
            continue
        # Courses for which a block or two periods a day is normal are excluded.
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
                detail=f"{Weekday(day).label} {subject_id} {count} periods (allowed {limit})",
            )
        )
    return InspectionItem(
        id="same_subject_twice_a_day",
        name="Same subject repeated in a day",
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
            detail=f"{Weekday(day).label} {len(periods)} periods",
        )
        for (teacher_id, day), periods in sorted(index.teacher_periods.items())
        if len(periods) > options.max_periods_per_day
    ]
    return InspectionItem(
        id="teacher_daily_overload",
        name=f"Teachers with more than {options.max_periods_per_day} periods",
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
            detail=f"{Weekday(day).label} {len(subjects)} subjects ({', '.join(sorted(subjects))})",
        )
        for (teacher_id, day), subjects in sorted(index.teacher_subjects.items())
        if len(subjects) >= options.max_subjects_per_day
    ]
    return InspectionItem(
        id="teacher_many_subjects",
        name=f"{options.max_subjects_per_day}+ subjects on one day",
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
                detail=f"{Weekday(day).label} {max(runs)} consecutive periods ({len(runs)} times)",
            )
        )
    return InspectionItem(
        id=item_id,
        name=f"{length}+ consecutive periods",
        severity="warning" if length >= options.long_run else "info",
        count=len(findings),
        findings=tuple(findings),
    )


def _lunch_adjacent(index: _Index, options: InspectOptions) -> InspectionItem:
    """Consecutive lessons across lunch (Comcigan's "consecutive around lunch")."""
    if options.lunch_after_period is None:
        return InspectionItem(id="lunch_adjacent", name="Consecutive around lunch", severity="info", count=0)
    before, after = options.lunch_after_period, options.lunch_after_period + 1
    findings = [
        Finding(
            subject_type="teacher",
            subject_id=teacher_id,
            subject_name=index.teacher_name(teacher_id),
            day=int(day),
            detail=f"{Weekday(day).label} periods {before}-{after} consecutive",
        )
        for (teacher_id, day), periods in sorted(index.teacher_periods.items())
        if before in periods and after in periods
    ]
    return InspectionItem(
        id="lunch_adjacent",
        name="Consecutive around lunch",
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
            detail=f"{Weekday(day).label} {gaps} gap periods",
        )
        for (teacher_id, day), periods in sorted(index.teacher_periods.items())
        if (gaps := _gap_count(periods))
    ]
    return InspectionItem(
        id="teacher_gap", name="Teacher gaps", severity="info", count=len(findings), findings=tuple(findings)
    )


def _class_gaps(index: _Index, _: InspectOptions) -> InspectionItem:
    """Empty cells in a class timetable. Counted with the per-grade periods per weekday."""
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
                        detail=f"{Weekday(day).label} {', '.join(f'P{p}' for p in missing)} empty",
                    )
                )
    return InspectionItem(
        id="class_empty_period",
        name="Empty class periods",
        severity="error",
        count=len(findings),
        findings=tuple(findings),
    )


def _workload_spread(index: _Index, options: InspectOptions) -> InspectionItem:
    """Comcigan's "teacher load balance" — teachers whose periods per weekday vary a lot."""
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
                    detail=f"per weekday {per_day} (max-min {spread})",
                )
            )
    return InspectionItem(
        id="workload_spread",
        name="Teacher load spread across weekdays",
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
            detail=f"{Weekday(day).label} is a requested free day but has {len(index.teacher_periods[teacher.id, day])} periods",
        )
        for teacher in index.problem.teachers
        for day in sorted(teacher.preferred_free_days)
        if index.teacher_periods.get((teacher.id, day))
    ]
    return InspectionItem(
        id="free_day_broken",
        name="Requested free day violated",
        severity="info",
        count=len(findings),
        findings=tuple(findings),
    )


def inspect_timetable(
    problem: TimetableProblem,
    timetable: Timetable,
    options: InspectOptions | None = None,
) -> InspectionReport:
    """Runs every search condition over the timetable and builds the report."""
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

    # A partial solution that allows unassigned lessons naturally shows H1 violations, so
    # those messages are filtered out (the `unassigned` item already shows the same, structured).
    violations = tuple(v for v in validate_timetable(problem, timetable) if "weekly periods" not in v)

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
