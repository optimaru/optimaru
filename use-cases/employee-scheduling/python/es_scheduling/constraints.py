"""Constraint specs — one class per constraint of the employee-scheduling engine.

Each spec names one C++ component (``name`` is its Python registry name, the
C++ name in snake_case) and carries the five knobs every optimaru constraint
takes, plus the constraint's own domain parameters::

    constraints = [
        RequiredSkillCost(),                         # hard, default weight
        MinRestCost(weight=0.25, level="soft"),        # demoted to a cheap soft cost
        UndesiredDayCost(weight=2.0, per_day=True),  # soft, double weight, 1 per day
        ShiftCountImbalanceCost(exponent=2.0),
        MaxConsecutiveDaysCost(value=5, worker_indices=(0, 1)),   # scoped
        DayPatternCost(pattern="Location A,OFF,Location B"),
        DayPatternCost(pattern="WORK,WORK,WORK,WORK,WORK,WORK,WORK"),  # same rule, again
        UndesiredDayCost(weight=0.01, exponent=2, offset=120),   # + (minutes beyond 2 h)^2
    ]
    solver.solve(schedule, DecomposedCalculator(get_constraints(constraints)))

The knobs (see ``_constraint.py``):

- ``weight``   ``None`` = the component's default weight (C++ kDefaultWeight;
               1 for every employee-scheduling constraint); otherwise a
               finite number >= 0.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level.
- ``exponent`` sign-preserving power shaping, > 0: per employee for a unit
               constraint, on the single total for a global one.
- ``offset``   (keyword) a shift of that shaping, any finite number (0.0 =
               none), at the same granularity.
- ``clip``     (keyword) ``True`` (the default, the threshold) counts only
               what lies beyond the offset, ``sign(c) * max(0, |c| - offset)
               ** exponent``; ``False`` (the literal formula) shapes ``c -
               offset`` as it is, which charges every employee the rule
               evaluates (a raw 0 included; a worker-scoped rule evaluates
               only the employees in its scope) and rewards a cost below the
               offset.

The defaults live in C++ only; ``es_scheduling.constraint_info()`` reports
them for all 30 constraints. Every other field is a domain parameter
(``spec.params()``), including the rule scope ``worker_indices`` /
``shift_indices`` (engine indices; ``None`` = everyone) of the unit
constraints and of ``ShiftCoverageCost``. A domain parameter's bounds are
declared once, in the class's ``bounds``, and checked here; the JSON rule
models (``es_scheduling.models``) are generated from these classes and turn
the same bounds into pydantic constraints.

One constraint may be picked several times, with different parameters, scope
or knobs (``x**2 + x**3`` is two picks, ``exponent=2`` and ``exponent=3``) or
with the same ones (identical picks simply count twice); the C++ factory
scores each instance on its own, ``constraint_totals()`` tells them apart by
label and ``constraint_costs()`` keys the k-th instance ``"<name>#<k>"``.
(The module also carries ready-made default instances: ``required_skill_cost``
is ``RequiredSkillCost()``, and so on.)

This module does not need pydantic.
"""

import operator
from dataclasses import dataclass, field
from typing import ClassVar, Literal

from ._constraint import Constraint, Level, Score, instance_keys, score_costs, select
from ._core import MAX_DAY

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "instance_keys",
    "RequiredSkillCost",
    "EmployeeUnavailabilityCost",
    "ShiftOverlapCost",
    "MaxOneShiftPerDayCost",
    "MinRestCost",
    "UndesiredDayCost",
    "DesiredDayCost",
    "DesiredShiftCost",
    "ShiftCountImbalanceCost",
    "MaxConsecutiveDaysCost",
    "MinConsecutiveDaysCost",
    "MaxConsecutiveDaysOffCost",
    "RestAfterConsecutiveWorkCost",
    "MaxWeeklyMinutesCost",
    "MinWeeklyMinutesCost",
    "MinWeeklyRestDaysCost",
    "AssignmentCost",
    "UnassignedShiftsCost",
    "ShiftCountGapCost",
    "WeeklyMinutesGapCost",
    "ShiftCoverageCost",
    "LocationRunLengthCost",
    "RestAfterLocationRunCost",
    "LocationRunGapCost",
    "WorkerLocationCountCost",
    "DayPatternCost",
    "TargetDaysOffCost",
    "WorkerDayCountInRangeCost",
    "LocationImbalanceCost",
    "WorkerPairingCost",
    "required_skill_cost",
    "employee_unavailability_cost",
    "shift_overlap_cost",
    "max_one_shift_per_day_cost",
    "min_rest_cost",
    "undesired_day_cost",
    "desired_day_cost",
    "desired_shift_cost",
    "shift_count_imbalance_cost",
    "max_consecutive_days_cost",
    "min_consecutive_days_cost",
    "max_consecutive_days_off_cost",
    "rest_after_consecutive_work_cost",
    "max_weekly_minutes_cost",
    "min_weekly_minutes_cost",
    "min_weekly_rest_days_cost",
    "assignment_cost",
    "unassigned_shifts_cost",
    "shift_count_gap_cost",
    "weekly_minutes_gap_cost",
    "shift_coverage_cost",
    "location_run_length_cost",
    "rest_after_location_run_cost",
    "location_run_gap_cost",
    "worker_location_count_cost",
    "day_pattern_cost",
    "target_days_off_cost",
    "worker_day_count_in_range_cost",
    "location_imbalance_cost",
    "worker_pairing_cost",
    "SPEC_CLASSES",
    "get_constraints",
]

_CHECKS = {
    "ge": (operator.ge, ">="),
    "gt": (operator.gt, ">"),
    "le": (operator.le, "<="),
    "lt": (operator.lt, "<"),
}


@dataclass(frozen=True)
class _EsConstraint(Constraint, abstract=True):
    """ES specs: the shared knobs plus domain parameters with declared bounds."""

    # {field: {"ge": 1}}: declared ONCE; enforced here and turned into pydantic
    # bounds (and JSON-schema minimum/maximum) by models.py.
    bounds: ClassVar[dict[str, dict[str, float]]] = {}

    def __post_init__(self):
        super().__post_init__()
        for field_name, limits in type(self).bounds.items():
            value = getattr(self, field_name)
            for op, limit in limits.items():
                check, symbol = _CHECKS[op]
                if value is not None and not check(value, limit):
                    raise ValueError(f"constraint {self.name!r}: {field_name} must be {symbol} {limit}, not {value!r}")


@dataclass(frozen=True)
class _Scoped(_EsConstraint, abstract=True):
    """A unit-constraint rule that may be restricted to workers / shifts.

    ``worker_indices`` / ``shift_indices`` are engine indices (positions in the
    schedule's employee / shift lists); ``None`` = no restriction on that
    dimension, an empty tuple = nobody / no shift. A worker scope skips the
    other employees entirely; a shift scope counts only the listed shifts.
    """

    worker_indices: tuple[int, ...] | None = None
    shift_indices: tuple[int, ...] | None = None


# ---------------------------------------------------------------------------
# The eight defaults
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class RequiredSkillCost(_Scoped):
    """1 per assigned shift whose required skill the employee lacks. Default: hard, weight 1."""

    name = "required_skill_cost"
    label_en = "Required skill"
    label_ko = "필수 기술"
    description_en = (
        "Assigned shifts whose required skill the employee lacks (pinned shifts are not judged again). "
        "One unit of cost is one such shift; 0 means every shift is staffed by someone with its skill."
    )
    description_ko = (
        "배정된 근무 가운데 맡은 직원에게 그 근무의 필수 기술이 없는 근무를 셉니다(고정 근무는 다시 따지지 않습니다). "
        "그런 근무 하나가 비용 1이며, 0이면 모든 근무를 그 기술을 가진 사람이 맡습니다."
    )


@dataclass(frozen=True)
class EmployeeUnavailabilityCost(_Scoped):
    """Minutes of assigned shifts on the employee's unavailable days. Default: hard, weight 1."""

    name = "employee_unavailability_cost"
    label_en = "Employee unavailability"
    label_ko = "근무 불가일"
    description_en = (
        "Minutes of assigned shifts that fall on a day the employee is unavailable (pinned shifts are not counted). "
        "One unit of cost is one minute; 0 means nobody works on a day they are unavailable."
    )
    description_ko = (
        "직원이 근무할 수 없는 날에 걸친 배정 근무 시간을 분 단위로 더합니다(고정 근무는 세지 않습니다). "
        "1분이 비용 1이며, 0이면 아무도 근무 불가일에 일하지 않습니다."
    )


@dataclass(frozen=True)
class ShiftOverlapCost(_Scoped):
    """Overlap minutes between two shifts of the same employee. Default: hard, weight 1."""

    name = "shift_overlap_cost"
    label_en = "Shift overlap"
    label_ko = "근무 시간 겹침"
    description_en = (
        "Minutes by which two shifts of the same employee overlap, summed over all pairs. "
        "One unit of cost is one overlapping minute; 0 means no employee holds overlapping shifts."
    )
    description_ko = (
        "같은 직원의 두 근무가 겹치는 시간을 모든 쌍에 대해 분 단위로 더합니다. "
        "겹치는 1분이 비용 1이며, 0이면 겹치는 근무를 맡은 직원이 없습니다."
    )


@dataclass(frozen=True)
class MaxOneShiftPerDayCost(_Scoped):
    """1 per PAIR of same-employee shifts starting on the same day. Default: hard, weight 1."""

    name = "max_one_shift_per_day_cost"
    label_en = "One shift per day"
    label_ko = "하루 한 근무"
    description_en = (
        "Pairs of shifts of the same employee that start on the same day (three shifts on one day are three pairs). "
        "One unit of cost is one pair; 0 means everyone works at most one shift a day."
    )
    description_ko = (
        "같은 날 시작하는 같은 직원의 근무 쌍을 셉니다(하루에 세 근무면 세 쌍입니다). "
        "한 쌍이 비용 1이며, 0이면 모두 하루에 한 근무 이하로 일합니다."
    )


@dataclass(frozen=True)
class MinRestCost(_Scoped):
    """Rest shortfall below `min_gap_minutes` (default 600 = 10 hours) between two
    same-employee shifts. Default: hard, weight 1."""

    name = "min_rest_cost"
    label_en = "Minimum rest"
    label_ko = "최소 휴식"
    description_en = (
        "Rest shortfall below min_gap_minutes (default 600, ten hours) between two shifts of the same employee, "
        "summed over all pairs; a zero gap counts. "
        "One unit of cost is one minute short; 0 means every two shifts are far enough apart."
    )
    description_ko = (
        "같은 직원의 두 근무 사이 휴식이 min_gap_minutes(기본 600분, 10시간)보다 모자란 시간을 "
        "모든 쌍에 대해 분 단위로 더합니다. 간격이 0이어도 셉니다. "
        "모자란 1분이 비용 1이며, 0이면 모든 근무 사이에 충분한 휴식이 있습니다."
    )
    bounds = {"min_gap_minutes": {"ge": 0}}
    min_gap_minutes: int = 600


@dataclass(frozen=True)
class UndesiredDayCost(_Scoped):
    """Minutes on the employee's undesired days, or 1 per such shift with `per_day`.
    Default: soft, weight 1."""

    name = "undesired_day_cost"
    label_en = "Undesired day"
    label_ko = "기피일 근무"
    description_en = (
        "Minutes of assigned shifts on a day the employee asked to avoid, or with per_day one per such shift "
        "(pinned shifts are not counted). "
        "One unit of cost is one minute (one shift with per_day); 0 means nobody works on a day they wanted off."
    )
    description_ko = (
        "직원이 피하고 싶다고 한 날에 배정된 근무 시간을 분 단위로 더하고, per_day이면 그런 근무를 하나씩 셉니다"
        "(고정 근무는 세지 않습니다). "
        "1분(per_day이면 근무 하나)이 비용 1이며, 0이면 아무도 기피일에 일하지 않습니다."
    )
    per_day: bool = False


@dataclass(frozen=True)
class DesiredDayCost(_Scoped):
    """Reward: minus the minutes on the employee's desired days, or minus 1 per such
    shift with `per_day` (raw <= 0; never hard). Default: soft, weight 1."""

    name = "desired_day_cost"
    label_en = "Desired day"
    label_ko = "희망일 근무"
    description_en = (
        "A reward: minus the minutes of assigned shifts on a day the employee asked to work, "
        "or with per_day minus one per such shift (pinned shifts are not counted). "
        "The raw cost is never positive; 0 means no desired day is worked. It cannot be made hard."
    )
    description_ko = (
        "보상 항목입니다. 직원이 일하고 싶다고 한 날에 배정된 근무 시간(per_day이면 근무 수)을 음수로 셉니다"
        "(고정 근무는 세지 않습니다). "
        "원시 비용은 0 이하이며, 0이면 희망일에 배정된 근무가 없습니다. 하드 제약으로 바꿀 수 없습니다."
    )
    per_day: bool = False


@dataclass(frozen=True)
class DesiredShiftCost(_Scoped):
    """Reward: minus 1 per assigned shift whose (day, location) the employee asked for
    in ``desired_shifts`` — the location must match, not just the day (raw <= 0;
    never hard). Default: soft, weight 1."""

    name = "desired_shift_cost"
    label_en = "Desired shift"
    label_ko = "희망 근무"
    description_en = (
        "A reward: minus one per assigned shift whose day and location the employee asked for (desired_shifts); "
        "another location on that day earns nothing. "
        "The raw cost is never positive; 0 means no wish is granted. It cannot be made hard."
    )
    description_ko = (
        "보상 항목입니다. 직원이 desired_shifts로 요청한 날짜와 장소가 모두 맞는 배정 근무마다 -1을 셉니다. "
        "같은 날 다른 장소는 해당하지 않습니다. "
        "원시 비용은 0 이하이며, 0이면 들어준 요청이 없습니다. 하드 제약으로 바꿀 수 없습니다."
    )


@dataclass(frozen=True)
class ShiftCountImbalanceCost(_EsConstraint):
    """Load-balancing unfairness of the per-employee shift counts, sqrt(sum of squared
    deviations) (global). Default: soft, weight 1."""

    name = "shift_count_imbalance_cost"
    label_en = "Shift count imbalance"
    label_ko = "근무 수 불균형"
    description_en = (
        "How unevenly the shifts are spread: the square root of the summed squared deviations of each employee's "
        "shift count from the mean, rounded to six significant figures; employees without a shift count too. "
        "0 means everyone holds the same number of shifts."
    )
    description_ko = (
        "근무가 직원들에게 얼마나 고르지 않게 나뉘었는지 나타냅니다. 직원별 근무 수가 평균에서 벗어난 정도의 "
        "제곱합에 제곱근을 씌운 값(유효숫자 6자리)이며, 근무가 없는 직원도 포함합니다. "
        "0이면 모두 같은 수의 근무를 맡습니다."
    )


# ---------------------------------------------------------------------------
# Rule-only constraints (not in the default set)
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class MaxConsecutiveDaysCost(_Scoped):
    """Days beyond `value` in any run of consecutive worked days (value=6: "at most 6
    days in a row"). Default: hard, weight 1."""

    name = "max_consecutive_days_cost"
    label_en = "Max consecutive work days"
    label_ko = "최대 연속 근무일"
    description_en = (
        "Days beyond value (default 6) in each run of consecutive worked days; "
        "a day counts once even with several shifts. "
        "One unit of cost is one day too many; 0 means nobody works more than value days in a row."
    )
    description_ko = (
        "연속으로 일한 날이 value(기본 6)를 넘는 일수를 근무 묶음마다 더합니다. "
        "하루에 근무가 여럿이어도 하루로 셉니다. "
        "초과한 하루가 비용 1이며, 0이면 아무도 value일보다 길게 연속 근무하지 않습니다."
    )
    bounds = {"value": {"ge": 1}}
    value: int = 6


@dataclass(frozen=True)
class MinConsecutiveDaysCost(_Scoped):
    """Shortfall below `value` of any run of consecutive worked days ("if you work,
    work at least `value` days in a row"). Default: soft, weight 1."""

    name = "min_consecutive_days_cost"
    label_en = "Min consecutive work days"
    label_ko = "최소 연속 근무일"
    description_en = (
        "Shortfall below value (default 2) of each run of consecutive worked days; "
        "a run that reaches the last scheduled day is waived. "
        "One unit of cost is one day short; 0 means every stretch of work lasts at least value days."
    )
    description_ko = (
        "연속 근무 묶음이 value(기본 2)일보다 짧을 때 모자란 일수를 더합니다. "
        "계획 기간의 마지막 날까지 이어지는 묶음은 제외합니다. "
        "모자란 하루가 비용 1이며, 0이면 모든 연속 근무가 value일 이상입니다."
    )
    bounds = {"value": {"ge": 1}}
    value: int = 2


@dataclass(frozen=True)
class MaxConsecutiveDaysOffCost(_Scoped):
    """Interior runs of more than `value` consecutive days off (bounded by work on both
    sides; off days at the horizon edge are not counted). Default: soft, weight 1."""

    name = "max_consecutive_days_off_cost"
    label_en = "Max days off in a row"
    label_ko = "최대 연속 휴무일"
    description_en = (
        "Days beyond value (default 3) in each run of days off that lies between two worked days; "
        "off days at either end of the horizon do not count. "
        "One unit of cost is one day off too many; 0 means no gap between worked days is longer than value."
    )
    description_ko = (
        "두 근무일 사이에 낀 연속 휴무가 value(기본 3)일을 넘는 일수를 더합니다. "
        "계획 기간 양 끝의 휴무는 세지 않습니다. "
        "초과한 하루가 비용 1이며, 0이면 근무 사이 휴무가 value일보다 길지 않습니다."
    )
    bounds = {"value": {"ge": 1}}
    value: int = 3


@dataclass(frozen=True)
class RestAfterConsecutiveWorkCost(_Scoped):
    """After >= `work_days` consecutive worked days, the shortfall of the following rest
    below `rest_days` (a run at the end of the horizon is not counted). Default: hard,
    weight 1."""

    name = "rest_after_consecutive_work_cost"
    label_en = "Rest after a work stretch"
    label_ko = "연속 근무 후 휴식"
    description_en = (
        "After work_days (default 6) or more worked days in a row, the employee needs rest_days (default 3) days off "
        "before working again; a run at the end of the horizon is not counted. "
        "One unit of cost is one rest day missing; 0 means every long stretch is followed by enough rest."
    )
    description_ko = (
        "work_days(기본 6)일 이상 연속으로 일한 뒤에는 다시 일하기 전에 rest_days(기본 3)일을 쉬어야 합니다. "
        "계획 기간 끝에 걸친 묶음은 세지 않습니다. "
        "모자란 휴식일 하루가 비용 1이며, 0이면 긴 연속 근무 뒤에 늘 충분히 쉽니다."
    )
    bounds = {"work_days": {"ge": 1}, "rest_days": {"ge": 1}}
    work_days: int = 6
    rest_days: int = 3


@dataclass(frozen=True)
class MaxWeeklyMinutesCost(_Scoped):
    """Assigned minutes beyond `minutes` in any calendar week (7-day blocks from
    `week_anchor`; a shift counts fully in its start week). 2400 = a 40-hour week.
    Default: hard, weight 1."""

    name = "max_weekly_minutes_cost"
    label_en = "Max minutes per week"
    label_ko = "주간 최대 근무 시간"
    description_en = (
        "Assigned minutes beyond minutes (default 2400, a 40-hour week) in each calendar week of 7 days "
        "from week_anchor (default day 0). One unit of cost is one minute over; 0 means no week is over the limit."
    )
    description_ko = (
        "week_anchor(기본 0일)부터 7일씩 나눈 주마다 배정 시간이 minutes(기본 2400분, 주 40시간)를 넘는 분을 "
        "더합니다. 초과한 1분이 비용 1이며, 0이면 한도를 넘는 주가 없습니다."
    )
    bounds = {"minutes": {"ge": 0}}
    minutes: int = 40 * 60
    # An engine day index on a week boundary; the 7-day blocks align to it.
    week_anchor: int = 0


@dataclass(frozen=True)
class MinWeeklyMinutesCost(_Scoped):
    """Shortfall below `minutes` in EVERY calendar week of the horizon (7-day blocks
    from `week_anchor`; an idle week costs the full threshold). Default: soft,
    weight 1."""

    name = "min_weekly_minutes_cost"
    label_en = "Min minutes per week"
    label_ko = "주간 최소 근무 시간"
    description_en = (
        "Shortfall below minutes (default 480) in every calendar week of 7 days from week_anchor (default day 0); "
        "an idle week costs the full amount. "
        "One unit of cost is one minute short; 0 means everyone reaches the weekly minimum."
    )
    description_ko = (
        "week_anchor(기본 0일)부터 7일씩 나눈 모든 주에서 배정 시간이 minutes(기본 480분)에 못 미치는 분을 "
        "더합니다. 근무가 없는 주는 minutes 전체를 셉니다. "
        "모자란 1분이 비용 1이며, 0이면 모두 주간 최소 시간을 채웁니다."
    )
    bounds = {"minutes": {"ge": 0}}
    minutes: int = 8 * 60
    week_anchor: int = 0


@dataclass(frozen=True)
class MinWeeklyRestDaysCost(_Scoped):
    """Worked days beyond ``7 - value`` in any calendar week (7-day blocks from day 0) —
    "at least `value` rest days per week". Default: hard, weight 1."""

    name = "min_weekly_rest_days_cost"
    label_en = "Min rest days per week"
    label_ko = "주간 최소 휴무일"
    description_en = (
        "Worked days beyond 7 - value in each calendar week of 7 days from day 0, "
        "that is fewer than value (default 1) days off; a day counts once even with several shifts. "
        "One unit of cost is one worked day too many; 0 means every week leaves value days off."
    )
    description_ko = (
        "0일부터 7일씩 나눈 주마다 근무일이 7 - value일을 넘는 일수, 즉 휴무일이 value(기본 1)일보다 모자란 "
        "일수를 더합니다. 하루에 근무가 여럿이어도 하루로 셉니다. "
        "한도를 넘은 근무일 하루가 비용 1이며, 0이면 모든 주에 value일 이상 쉽니다."
    )
    bounds = {"value": {"ge": 0, "le": 7}}
    value: int = 1


@dataclass(frozen=True)
class AssignmentCost(_Scoped):
    """Money: per (employee, shift) the `shift_costs[employee_name][shift_id]` entry if
    present, else `cost_per_hour` * hours, else `cost_per_shift`. Default: soft,
    weight 1."""

    name = "assignment_cost"
    label_en = "Assignment cost"
    label_ko = "배정 비용"
    description_en = (
        "Money spent on the roster: per assigned shift, the shift_costs[employee][shift] entry if there is one, "
        "else cost_per_hour times the shift's hours, else cost_per_shift. "
        "One unit of cost is one unit of money; 0 means the roster costs nothing (always so at the default rates of 0)."
    )
    description_ko = (
        "근무표에 드는 비용입니다. 배정된 근무마다 shift_costs[직원][근무] 값이 있으면 그 값을, 없으면 "
        "cost_per_hour에 근무 시간을 곱한 값을, 그것도 없으면 cost_per_shift를 더합니다. "
        "금액 1이 비용 1이며, 0이면 비용이 들지 않습니다(기본 단가 0이면 늘 0입니다)."
    )
    cost_per_hour: float = 0.0
    cost_per_shift: float = 0.0
    # {employee name: {shift id: cost}}
    shift_costs: dict[str, dict[str, float]] = field(default_factory=dict)


@dataclass(frozen=True)
class UnassignedShiftsCost(_EsConstraint):
    """1 per shift with no employee assigned (coverage guarantee; global). Default:
    hard, weight 1."""

    name = "unassigned_shifts_cost"
    label_en = "Shifts left unassigned"
    label_ko = "미배정 근무"
    description_en = (
        "Shifts left without an employee. One unit of cost is one empty shift; 0 means every shift is covered."
    )
    description_ko = "직원이 배정되지 않은 근무를 셉니다. 빈 근무 하나가 비용 1이며, 0이면 모든 근무가 채워졌습니다."


@dataclass(frozen=True)
class ShiftCountGapCost(_EsConstraint):
    """Busiest-vs-least-busy shift-count gap beyond `tolerance` (global). Default: soft,
    weight 1."""

    name = "shift_count_gap_cost"
    label_en = "Shift count gap"
    label_ko = "근무 수 격차"
    description_en = (
        "How far the busiest employee's shift count is above the least busy one's, beyond tolerance (default 0). "
        "One unit of cost is one shift of gap; 0 means the counts are within tolerance of each other."
    )
    description_ko = (
        "근무가 가장 많은 직원과 가장 적은 직원의 근무 수 차이가 tolerance(기본 0)를 넘는 만큼입니다. "
        "허용치를 넘는 근무 수 차이 1이 비용 1이며, 0이면 근무 수가 허용 범위 안에 있습니다."
    )
    bounds = {"tolerance": {"ge": 0}}
    tolerance: int = 0


@dataclass(frozen=True)
class WeeklyMinutesGapCost(_EsConstraint):
    """Per calendar week, the busiest-vs-least-busy gap of assigned minutes beyond
    `tolerance_minutes`, in HOURS (global; all employees compared). Default: soft,
    weight 1."""

    name = "weekly_minutes_gap_cost"
    label_en = "Weekly hours gap"
    label_ko = "주간 근무 시간 격차"
    description_en = (
        "Per calendar week of 7 days from day 0, the gap between the busiest and the least busy employee's assigned "
        "minutes beyond tolerance_minutes (default 120), in hours, summed over the weeks; everyone is compared, "
        "so an idle employee counts as 0. One unit of cost is one hour of excess gap; 0 means every week is within "
        "tolerance."
    )
    description_ko = (
        "0일부터 7일씩 나눈 주마다 가장 많이 일한 직원과 가장 적게 일한 직원의 배정 시간 차이가 "
        "tolerance_minutes(기본 120분)를 넘는 만큼을 시간 단위로 바꿔 모든 주에 대해 더합니다. 모든 직원을 "
        "비교하므로 일이 없는 직원은 0으로 셉니다. 초과 격차 1시간이 비용 1이며, 0이면 모든 주가 허용 범위 안에 "
        "있습니다."
    )
    bounds = {"tolerance_minutes": {"ge": 0}}
    tolerance_minutes: int = 120


@dataclass(frozen=True)
class ShiftCoverageCost(_EsConstraint):
    """Per (day, location) group, the count of assignees holding `worker_skill` (empty =
    anyone) outside [`min_count`, `max_count`] (max_count < 0 = no cap; global).
    Default: soft, weight 1.

    Scoped natively: ``shift_indices`` restrict which shifts form groups and
    ``worker_indices`` which employees count; here ``None`` OR an empty tuple
    means everyone / every shift."""

    name = "shift_coverage_cost"
    label_en = "Shift coverage"
    label_ko = "근무 인원 충족"
    description_en = (
        "Per day and location, the assigned shifts held by employees with worker_skill (empty: anyone) below "
        "min_count or above max_count (-1: no cap). "
        "One unit of cost is one person missing or too many; 0 means every day and location is staffed within range."
    )
    description_ko = (
        "날짜와 장소별로 worker_skill을 가진 직원(비우면 누구나)이 맡은 배정 근무 수가 min_count보다 적거나 "
        "max_count(-1이면 상한 없음)보다 많은 만큼을 더합니다. "
        "모자라거나 남는 한 명이 비용 1이며, 0이면 모든 날짜와 장소의 인원이 범위 안에 있습니다."
    )
    bounds = {"min_count": {"ge": 0}, "max_count": {"ge": -1}}
    worker_skill: str = ""
    min_count: int = 0
    max_count: int = -1
    worker_indices: tuple[int, ...] | None = None
    shift_indices: tuple[int, ...] | None = None


@dataclass(frozen=True)
class LocationRunLengthCost(_Scoped):
    """Each maximal run of days at `location` (empty = any work) within [`min_run`,
    `max_run`] (max_run < 0 = no cap); shorter/longer runs cost the shortfall/excess.
    Default: soft, weight 1."""

    name = "location_run_length_cost"
    label_en = "Location run length"
    label_ko = "장소 연속 근무 길이"
    description_en = (
        "Each run of consecutive days an employee works at location (empty: any work) should last min_run to "
        "max_run days (-1: no cap); the cost is the shortfall or excess per run. "
        "One unit of cost is one day; 0 means every run is within bounds."
    )
    description_ko = (
        "직원이 location(비우면 모든 근무)에서 연속으로 일한 묶음은 min_run일에서 max_run일(-1이면 상한 없음) "
        "사이여야 하며, 묶음마다 모자라거나 넘는 일수를 비용으로 셉니다. "
        "하루가 비용 1이며, 0이면 모든 묶음이 범위 안에 있습니다."
    )
    bounds = {"min_run": {"ge": 0}, "max_run": {"ge": -1}}
    location: str = ""
    min_run: int = 0
    max_run: int = -1
    # Whether a run reaching the end of the horizon waives its min-run shortfall
    # (it might continue past the schedule). Set False when the rule stays in
    # force past the last scheduled day, so an end run shorter than min_run is
    # still a violation.
    waive_min_at_end: bool = True


@dataclass(frozen=True)
class RestAfterLocationRunCost(_Scoped):
    """After any run of `location` days, the worked days among the next `rest_days`
    days (which must be fully off). Default: hard, weight 1."""

    name = "rest_after_location_run_cost"
    label_en = "Rest after location run"
    label_ko = "장소 근무 후 휴식"
    description_en = (
        "After each run of days at location (empty: any work) the next rest_days (default 1) days must be fully off; "
        "the cost is the worked days inside those windows. "
        "One unit of cost is one day worked that should be off; 0 means every stint is followed by its rest."
    )
    description_ko = (
        "location(비우면 모든 근무)에서 연속으로 일한 뒤 이어지는 rest_days(기본 1)일은 완전히 쉬어야 하며, "
        "그 기간에 일한 날을 비용으로 셉니다. "
        "쉬어야 할 날에 일한 하루가 비용 1이며, 0이면 모든 근무 묶음 뒤에 휴식이 지켜집니다."
    )
    bounds = {"rest_days": {"ge": 1}}
    location: str = ""
    rest_days: int = 1


@dataclass(frozen=True)
class LocationRunGapCost(_Scoped):
    """Consecutive runs of `location` at least `min_gap` days apart; the shortfall per
    too-close pair. Default: soft, weight 1."""

    name = "location_run_gap_cost"
    label_en = "Location run gap"
    label_ko = "장소 근무 간격"
    description_en = (
        "Two consecutive runs of days at location (empty: any work) should be at least min_gap days apart; "
        "the cost is the missing days per pair of runs. "
        "One unit of cost is one day too close; 0 means the stints are far enough apart."
    )
    description_ko = (
        "location(비우면 모든 근무)에서 연속으로 일한 묶음 둘 사이는 min_gap일 이상 떨어져야 하며, 묶음 쌍마다 "
        "모자란 일수를 비용으로 셉니다. "
        "모자란 하루가 비용 1이며, 0이면 근무 묶음 사이 간격이 충분합니다."
    )
    bounds = {"min_gap": {"ge": 0}}
    location: str = ""
    min_gap: int = 0


@dataclass(frozen=True)
class WorkerLocationCountCost(_Scoped):
    """Per employee holding `worker_skill` (empty = all), the count of `location`
    shifts over the horizon outside [`min_count`, `max_count`] (max_count < 0 = no
    cap). Default: soft, weight 1."""

    name = "worker_location_count_cost"
    label_en = "Worker location count"
    label_ko = "직원별 장소 근무 수"
    description_en = (
        "Per employee with worker_skill (empty: everyone), the number of shifts at location (empty: any) over the "
        "horizon should be min_count to max_count (-1: no cap); pinned shifts do not count. "
        "One unit of cost is one shift short or over; 0 means everyone is within bounds."
    )
    description_ko = (
        "worker_skill을 가진 직원(비우면 모두)마다 계획 기간 동안 location(비우면 모든 장소)에서 맡은 근무 수가 "
        "min_count에서 max_count(-1이면 상한 없음) 사이여야 합니다. 고정 근무는 세지 않습니다. "
        "모자라거나 넘는 근무 하나가 비용 1이며, 0이면 모두 범위 안에 있습니다."
    )
    bounds = {"min_count": {"ge": 0}, "max_count": {"ge": -1}}
    location: str = ""
    worker_skill: str = ""
    min_count: int = 0
    max_count: int = -1


@dataclass(frozen=True)
class DayPatternCost(_Scoped):
    """1 per window matching an ordered day `pattern`: comma-separated tokens, each a
    location, "OFF" (off day), "*" (any day), "WORK" (any worked day) or "!x" (a
    worked day not at location x). Example: "Location A,OFF,Location B". Default:
    soft, weight 1."""

    name = "day_pattern_cost"
    label_en = "Forbidden day pattern"
    label_ko = "금지 근무 패턴"
    description_en = (
        "Occurrences of a forbidden sequence of days in an employee's roster. pattern is a comma-separated list of "
        "tokens: a location, OFF (no work), * (any day), WORK (any worked day) or !x (a worked day not at location x). "
        "One unit of cost is one matching window; 0 means the pattern never occurs."
    )
    description_ko = (
        "직원 근무표에 금지된 근무 패턴(연이은 날들의 순서)이 나타나는 횟수를 셉니다. "
        "pattern은 쉼표로 구분한 토큰 목록이며, 각 토큰은 장소, OFF(휴무), *(아무 날), WORK(근무한 날), "
        "!x(장소 x가 아닌 곳에서 근무한 날) 가운데 하나입니다. "
        "일치하는 구간 하나가 비용 1이며, 0이면 패턴이 나타나지 않습니다."
    )
    pattern: str = ""


@dataclass(frozen=True)
class TargetDaysOffCost(_Scoped):
    """Per employee, the shortfall below `min_count` days off among `target_days` (day
    indices, e.g. weekends / holidays). Default: soft, weight 1."""

    name = "target_days_off_cost"
    label_en = "Target days off"
    label_ko = "지정 휴무일"
    description_en = (
        "Per employee, at least min_count of target_days (day indices such as weekends and holidays) should be off; "
        "the cost is the shortfall. "
        "One unit of cost is one target day off missing; 0 means everyone gets enough of them off."
    )
    description_ko = (
        "직원마다 target_days(주말이나 공휴일 같은 날짜 번호) 가운데 적어도 min_count일은 쉬어야 하며, "
        "모자란 일수를 비용으로 셉니다. "
        "모자란 휴무일 하루가 비용 1이며, 0이면 모두 충분히 쉽니다."
    )
    bounds = {"min_count": {"ge": 0}}
    target_days: tuple[int, ...] = ()
    min_count: int = 0


@dataclass(frozen=True)
class WorkerDayCountInRangeCost(_Scoped):
    """Distinct worked days (at `location`; empty = any) in the day range [`lo`, `hi`]
    outside [`min_count`, `max_count`] (max_count < 0 = no cap). Usually scoped to
    one worker via ``worker_indices``: "off >= K over a window" == "worked <=
    window length - K". Default: soft, weight 1."""

    name = "worker_day_count_in_range_cost"
    label_en = "Worked days in a range"
    label_ko = "기간 내 근무일 수"
    description_en = (
        "Per employee, the distinct days worked (at location; empty: any) between days lo and hi should be min_count "
        "to max_count (-1: no cap); usually scoped to one employee and window. "
        "One unit of cost is one day short or over; 0 means within bounds."
    )
    description_ko = (
        "직원마다 lo일부터 hi일까지 location(비우면 모든 장소)에서 일한 날 수가 min_count에서 "
        "max_count(-1이면 상한 없음) 사이여야 하며, 하루에 근무가 여럿이어도 하루로 셉니다. "
        "보통 직원 한 명과 기간 하나로 범위를 정해 씁니다. "
        "모자라거나 넘는 하루가 비용 1이며, 0이면 범위 안에 있습니다."
    )
    bounds = {
        "lo": {"ge": 0, "le": MAX_DAY},
        "hi": {"ge": -1, "le": MAX_DAY},
        "min_count": {"ge": 0},
        "max_count": {"ge": -1},
    }
    location: str = ""  # empty = any work; else count only that location's days
    lo: int = 0
    hi: int = -1
    min_count: int = 0
    max_count: int = -1


@dataclass(frozen=True)
class LocationImbalanceCost(_EsConstraint):
    """Fairness of a per-employee count across employees holding `worker_skill` (empty =
    all): `location` shifts (empty = all work), or the days off among
    `off_target_days` when given. `metric` "ssd" (sqrt of squared deviations) or
    "gap" (max - min beyond `tolerance`; global). Default: soft, weight 1."""

    name = "location_imbalance_cost"
    label_en = "Location imbalance"
    label_ko = "장소 근무 불균형"
    description_en = (
        "How unevenly a count is spread over the employees with worker_skill (empty: everyone): shifts at location "
        "(empty: all work), or with off_target_days the days off among those days. metric 'ssd' is the square root "
        "of the summed squared deviations, 'gap' the max-min difference beyond tolerance. 0 means the counts are even."
    )
    description_ko = (
        "worker_skill을 가진 직원(비우면 모두) 사이에서 어떤 수가 얼마나 고르지 않은지 나타냅니다. 세는 대상은 "
        "location(비우면 모든 근무)의 근무 수이며, off_target_days를 주면 그 날들 가운데 쉰 날 수입니다. "
        "metric이 'ssd'이면 편차 제곱합의 제곱근, 'gap'이면 최댓값과 최솟값의 차이가 tolerance를 넘는 만큼입니다. "
        "0이면 고르게 나뉘었습니다."
    )
    bounds = {"tolerance": {"ge": 0}}
    location: str = ""
    worker_skill: str = ""
    metric: Literal["ssd", "gap"] = "ssd"
    tolerance: int = 0
    off_target_days: tuple[int, ...] = ()


@dataclass(frozen=True)
class WorkerPairingCost(_EsConstraint):
    """A directional worker pairing over the (``bucket_days``, ``bucket_locations``)
    buckets both workers are eligible for. ``penalty=False``: they should share the
    same shift, so a bucket where exactly one works it costs 1; ``penalty=True``:
    they should not, so a bucket where both work it costs 1. Raw >= 0 in both modes
    (not a reward). ``anchor`` / ``paired`` are engine worker indices (global).
    Default: soft, weight 1."""

    name = "worker_pairing_cost"
    label_en = "Worker pairing"
    label_ko = "직원 짝 근무"
    description_en = (
        "Over the given (bucket_days, bucket_locations) buckets, whether anchor and paired work together: with "
        "penalty=False they should share, so a bucket where only one of them works costs 1; with penalty=True they "
        "should not, so a bucket where both work costs 1. "
        "The raw cost is never negative; 0 means the pairing holds everywhere."
    )
    description_ko = (
        "bucket_days와 bucket_locations로 주어진 (날짜, 장소) 칸마다 anchor와 paired 직원이 함께 일하는지 봅니다. "
        "penalty=False이면 함께 일해야 하므로 한 명만 일한 칸이 비용 1이고, penalty=True이면 함께 일하면 안 되므로 "
        "둘 다 일한 칸이 비용 1입니다. 원시 비용은 음수가 아니며, 0이면 모든 칸에서 조건이 지켜집니다."
    )
    anchor: int = -1
    paired: int = -1
    bucket_days: tuple[int, ...] = ()
    bucket_locations: tuple[str, ...] = ()
    penalty: bool = False


#: Every spec, in the C++ factory order (= ``CONSTRAINT_NAMES``).
SPEC_CLASSES: tuple[type[Constraint], ...] = (
    RequiredSkillCost,
    EmployeeUnavailabilityCost,
    ShiftOverlapCost,
    MaxOneShiftPerDayCost,
    MinRestCost,
    UndesiredDayCost,
    DesiredDayCost,
    DesiredShiftCost,
    ShiftCountImbalanceCost,
    MaxConsecutiveDaysCost,
    MinConsecutiveDaysCost,
    MaxConsecutiveDaysOffCost,
    RestAfterConsecutiveWorkCost,
    MaxWeeklyMinutesCost,
    MinWeeklyMinutesCost,
    MinWeeklyRestDaysCost,
    AssignmentCost,
    UnassignedShiftsCost,
    ShiftCountGapCost,
    WeeklyMinutesGapCost,
    ShiftCoverageCost,
    LocationRunLengthCost,
    RestAfterLocationRunCost,
    LocationRunGapCost,
    WorkerLocationCountCost,
    DayPatternCost,
    TargetDaysOffCost,
    WorkerDayCountInRangeCost,
    LocationImbalanceCost,
    WorkerPairingCost,
)

#: Ready-made default instances at the default weights and parameters, one per
#: constraint (``required_skill_cost`` is ``RequiredSkillCost()``).
required_skill_cost = RequiredSkillCost()
employee_unavailability_cost = EmployeeUnavailabilityCost()
shift_overlap_cost = ShiftOverlapCost()
max_one_shift_per_day_cost = MaxOneShiftPerDayCost()
min_rest_cost = MinRestCost()
undesired_day_cost = UndesiredDayCost()
desired_day_cost = DesiredDayCost()
desired_shift_cost = DesiredShiftCost()
shift_count_imbalance_cost = ShiftCountImbalanceCost()
max_consecutive_days_cost = MaxConsecutiveDaysCost()
min_consecutive_days_cost = MinConsecutiveDaysCost()
max_consecutive_days_off_cost = MaxConsecutiveDaysOffCost()
rest_after_consecutive_work_cost = RestAfterConsecutiveWorkCost()
max_weekly_minutes_cost = MaxWeeklyMinutesCost()
min_weekly_minutes_cost = MinWeeklyMinutesCost()
min_weekly_rest_days_cost = MinWeeklyRestDaysCost()
assignment_cost = AssignmentCost()
unassigned_shifts_cost = UnassignedShiftsCost()
shift_count_gap_cost = ShiftCountGapCost()
weekly_minutes_gap_cost = WeeklyMinutesGapCost()
shift_coverage_cost = ShiftCoverageCost()
location_run_length_cost = LocationRunLengthCost()
rest_after_location_run_cost = RestAfterLocationRunCost()
location_run_gap_cost = LocationRunGapCost()
worker_location_count_cost = WorkerLocationCountCost()
day_pattern_cost = DayPatternCost()
target_days_off_cost = TargetDaysOffCost()
worker_day_count_in_range_cost = WorkerDayCountInRangeCost()
location_imbalance_cost = LocationImbalanceCost()
worker_pairing_cost = WorkerPairingCost()

#: The default set, in the order ``es::defaultConstraints()`` builds it (the
#: first rows of ``constraint_info()``). The other 22 are opt-in rules.
_DEFAULTS: tuple[Constraint, ...] = (
    required_skill_cost,
    employee_unavailability_cost,
    shift_overlap_cost,
    max_one_shift_per_day_cost,
    min_rest_cost,
    undesired_day_cost,
    desired_day_cost,
    shift_count_imbalance_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """The cost function, as a list of specs. No argument = the eight defaults.

    The same entry point every use case in this repo exposes, so "start from
    the defaults and change one thing" reads the same everywhere::

        from es_scheduling.constraints import UndesiredDayCost, get_constraints

        solver.solve(schedule, DecomposedCalculator(get_constraints()))
        solver.solve(schedule, DecomposedCalculator(get_constraints([
            RequiredSkillCost(), UndesiredDayCost(weight=2.0),
        ])))

    Passing `constraints` uses that list as given — only what is in it is
    scored. An empty list is an error, not "the defaults". A constraint may
    appear several times, with different parameters (scope included) or
    knobs, or with the same ones: every pick is its own instance, scored on
    its own, and the instances add up.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Score raw constraint costs with a selection — pure Python, the engine's arithmetic.

    `costs` must be ``constraint_costs(schedule, constraints)`` of the SAME
    list (repeated rules are keyed ``"<name>#<k>"``); None = the defaults on
    both sides. Each spec's weight / level / exponent / offset / clip re-prices
    its entry without another scoring pass. A repeat of a rule without domain
    parameters may also be scored from costs that hold it once (the same raw
    costs); a parametrized repeat needs its own ``"<name>#<k>"`` entry.
    """
    return score_costs(costs, get_constraints(constraints))
