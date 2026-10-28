"""Penalty expressions used by the soft constraints.

Rules that are good to keep but leave the timetable usable when broken are
turned into penalties. Which terms are used with which weights is decided by
the constraint classes in `registry`; this module only builds the expressions
they call.

S1 minimise teacher gaps        idle periods between a day's first and last lesson
S2 minimise teacher workdays    give teachers days without lessons
S3 respect requested free days
S4 minimise class gaps          no holes in the middle of a class's day
S5 avoid avoided periods        e.g. PE not in period 1 or period 5 (right after lunch)
S6 favour preferred periods     e.g. core subjects in the morning
"""

from dataclasses import dataclass

from ortools.sat.python import cp_model

from teacher_scheduling.solver.variables import TimetableVariables

PenaltyTerms = dict[str, cp_model.LinearExpr]


@dataclass(frozen=True)
class DaySpan:
    """Shape of one day's timetable. working = any lesson that day, gap = empty cells between the first and last lesson."""

    working: cp_model.IntVar
    gap: cp_model.IntVar


def _sum_or_none(terms: list) -> cp_model.LinearExpr | None:
    return cp_model.LinearExpr.sum(terms) if terms else None


# ---------------------------------------------------------------- per-day shape


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
    """gaps = (last period - first period + 1) - actual lessons.

    A period excluded from the candidates (personal reasons, say) that falls
    between the first and last lesson is counted as a gap too — it is an empty
    cell in the real timetable as well.
    """
    periods = sorted(busy_by_period)
    if not periods:  # no assignable cell on this day
        return None

    busy_vars = [busy_by_period[period] for period in periods]
    last_period = periods[-1]

    working = model.new_bool_var(f"{name}_working")
    model.add_max_equality(working, busy_vars)

    if last_period - periods[0] < 2:  # a candidate span of two cells or less cannot have a gap in the middle
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


# ---------------------------------------------------------------- individual penalty terms


def _free_day_violations(variables: TimetableVariables, spans: dict[tuple[str, int], DaySpan]) -> list[cp_model.IntVar]:
    return [
        span.working
        for (teacher_id, day), span in spans.items()
        if day in variables.problem.teacher_by_id[teacher_id].preferred_free_days
    ]


def _period_preference_vars(variables: TimetableVariables, *, avoided: bool) -> list[cp_model.IntVar]:
    """avoided=True collects placements in avoided periods, False placements outside the preferred periods."""
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
