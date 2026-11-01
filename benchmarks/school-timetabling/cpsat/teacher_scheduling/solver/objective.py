"""Penalty expressions used by the soft constraints.

Rules that are good to keep but leave the timetable usable when broken are
turned into penalties. Which terms are used with which weights is decided by
the constraint classes in `registry`; this module only builds the expressions
they call.

teacher_gap_cost           idle periods between a day's first and last lesson
teacher_workday_cost       give teachers days without lessons
teacher_free_day_cost      respect requested free days (worked requested free days)
class_gap_cost             no holes in the middle of a class's day
avoided_period_cost        e.g. PE not in period 1 or period 5 (right after lunch)
non_preferred_period_cost  e.g. core subjects in the morning
"""

from dataclasses import dataclass

from ortools.sat.python import cp_model

from teacher_scheduling.solver.variables import TimetableVariables

PenaltyTerms = dict[str, cp_model.LinearExpr]

# A safe upper bound for the shaped term of a penalty count (the counts here
# are far below it); CP-SAT needs one for the auxiliary variable.
_SHAPED_BOUND = 10**9


@dataclass(frozen=True)
class DaySpan:
    """Shape of one day's timetable. working = any lesson that day, gap = empty cells between the first and last lesson."""

    working: cp_model.IntVar
    gap: cp_model.IntVar


def _sum_or_none(terms: list) -> cp_model.LinearExpr | None:
    return cp_model.LinearExpr.sum(terms) if terms else None


def shaped(model: cp_model.CpModel, value, offset: int, clip: bool):
    """The engine's shapeCost at exponent 1 of a non-negative penalty count ``value``.

    ``clip`` (the threshold): ``max(0, value - offset)`` for an offset > 0,
    ``value + |offset| * [value >= 1]`` for an offset < 0 (a 0 stays 0);
    otherwise (the literal formula) ``value - offset``. Offset 0 returns
    ``value`` itself. Exact on integral values and offsets; an int ``value``
    (a unit no variable can reach) gives an int.
    """
    if offset == 0:
        return value
    if not clip:
        return value - offset
    if isinstance(value, int):
        if value == 0:
            return 0
        return max(0, value - offset) if offset > 0 else value - offset
    if offset > 0:
        excess = model.new_int_var(0, _SHAPED_BOUND, "shaped")
        model.add_max_equality(excess, [0, value - offset])
        return excess
    positive = model.new_bool_var("shaped_positive")
    model.add(value >= 1).only_enforce_if(positive)
    model.add(value <= 0).only_enforce_if(~positive)
    return value + (-offset) * positive


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
    return [var for slot_vars in _period_preference_by_slot(variables, avoided=avoided).values() for var in slot_vars]


def _period_preference_by_slot(variables: TimetableVariables, *, avoided: bool) -> dict[object, list[cp_model.IntVar]]:
    """The placements `_period_preference_vars` collects, grouped by their time slot."""
    problem = variables.problem
    collected: dict[object, list[cp_model.IntVar]] = {}
    for course in problem.courses:
        subject = problem.subject_by_id[course.subject_id]
        for slot in variables.allowed_slots[course.id]:
            if avoided:
                hit = slot.period in subject.avoided_periods
            else:
                hit = bool(subject.preferred_periods) and slot.period not in subject.preferred_periods
            if hit:
                collected.setdefault(slot, []).append(variables.x[course.id, slot])
    return collected


def _period_preference_units(variables: TimetableVariables, *, avoided: bool) -> list:
    """One penalty count per calendar slot (the engine's unit of the per-slot constraints),
    every slot included: a slot nothing can be placed in is a constant 0."""
    by_slot = _period_preference_by_slot(variables, avoided=avoided)
    return [
        cp_model.LinearExpr.sum(by_slot[slot]) if slot in by_slot else 0 for slot in variables.problem.calendar.slots
    ]
