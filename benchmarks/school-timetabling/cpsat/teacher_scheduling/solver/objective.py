"""소프트 제약이 쓰는 벌점 식 구현.

지키면 좋지만 못 지켜도 시간표는 돌아가는 규칙들을 벌점으로 환산한다. 어떤
항목을 어떤 가중치로 쓸지는 `registry` 의 제약 클래스가 정하고, 여기서는 그
클래스들이 부르는 식만 만든다.

S1 교사 공강 최소화        하루 첫 수업과 마지막 수업 사이의 빈 교시
S2 교사 출근일 최소화      수업 없는 날을 만들어 주기
S3 교사 희망 공강일 존중
S4 학급 공강 최소화        학급 시간표 중간이 비지 않게
S5 과목 기피 교시 회피     예: 체육 1교시, 5교시(점심 직후) 회피
S6 과목 선호 교시 유도     예: 국영수는 오전
"""

from dataclasses import dataclass

from ortools.sat.python import cp_model

from teacher_scheduling.solver.variables import TimetableVariables

PenaltyTerms = dict[str, cp_model.LinearExpr]


@dataclass(frozen=True)
class DaySpan:
    """하루치 시간표의 모양. working=그날 수업이 있는지, gap=첫 수업~마지막 수업 사이 빈 칸 수."""

    working: cp_model.IntVar
    gap: cp_model.IntVar


def _sum_or_none(terms: list) -> cp_model.LinearExpr | None:
    return cp_model.LinearExpr.sum(terms) if terms else None


# ---------------------------------------------------------------- 하루 단위 모양


def _build_teacher_day_spans(model: cp_model.CpModel, variables: TimetableVariables) -> dict[tuple[str, int], DaySpan]:
    spans = {}
    for teacher in variables.problem.teachers:
        for day in variables.problem.calendar.days:
            busy = variables.teacher_busy_on_day(teacher.id, day)
            span = _day_span(model, busy, f"teacher[{teacher.id},{day.label}]")
            if span is not None:
                spans[teacher.id, day] = span
    return spans


def _build_class_day_spans(model: cp_model.CpModel, variables: TimetableVariables) -> dict[tuple[str, int], DaySpan]:
    spans = {}
    for class_group in variables.problem.class_groups:
        for day in variables.problem.calendar.days:
            busy = variables.class_busy_on_day(class_group.id, day)
            span = _day_span(model, busy, f"class[{class_group.id},{day.label}]")
            if span is not None:
                spans[class_group.id, day] = span
    return spans


def _day_span(model: cp_model.CpModel, busy_by_period: dict[int, cp_model.IntVar], name: str) -> DaySpan | None:
    """공강 수 = (마지막 교시 - 첫 교시 + 1) - 실제 수업 수.

    개인 사정 등으로 배정 후보에서 빠진 교시가 첫/마지막 수업 사이에 끼면 그 칸도
    공강으로 센다(실제 시간표에서도 비는 칸이므로).
    """
    periods = sorted(busy_by_period)
    if not periods:  # 이날은 배정 가능한 칸이 없다
        return None

    busy_vars = [busy_by_period[period] for period in periods]
    last_period = periods[-1]

    working = model.new_bool_var(f"{name}_working")
    model.add_max_equality(working, busy_vars)

    if last_period - periods[0] < 2:  # 후보 교시 폭이 두 칸 이하면 중간 공강이 생길 수 없다
        return DaySpan(working=working, gap=model.new_constant(0))

    first = model.new_int_var(0, last_period + 1, f"{name}_first")
    last = model.new_int_var(0, last_period, f"{name}_last")
    model.add_min_equality(
        first,
        [period * busy_by_period[period] + (last_period + 1) * (1 - busy_by_period[period]) for period in periods],
    )
    model.add_max_equality(last, [period * busy_by_period[period] for period in periods])

    gap = model.new_int_var(0, last_period, f"{name}_gap")
    model.add(gap == last - first + 1 - cp_model.LinearExpr.sum(busy_vars)).only_enforce_if(working)
    model.add(gap == 0).only_enforce_if(~working)
    return DaySpan(working=working, gap=gap)


# ---------------------------------------------------------------- 개별 벌점 항목


def _free_day_violations(variables: TimetableVariables, spans: dict[tuple[str, int], DaySpan]) -> list[cp_model.IntVar]:
    return [
        span.working
        for (teacher_id, day), span in spans.items()
        if day in variables.problem.teacher_by_id[teacher_id].preferred_free_days
    ]


def _period_preference_vars(variables: TimetableVariables, *, avoided: bool) -> list[cp_model.IntVar]:
    """avoided=True 면 기피 교시 배치, False 면 선호 교시를 벗어난 배치를 모은다."""
    problem = variables.problem
    collected = []
    for course in problem.courses:
        subject = problem.subject_by_id[course.subject_id]
        for slot in variables.allowed_slots[course.id]:
            if avoided:
                hit = slot.period in subject.avoided_periods
            else:
                hit = bool(subject.preferred_periods) and slot.period not in subject.preferred_periods
            if hit:
                collected.append(variables.x[course.id, slot])
    return collected
