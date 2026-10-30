"""Constraint spec classes — the intuitive way to assemble a cost function.

Instead of string names::

    calc = DecomposedCalculator()
    calc.add("required_skill", weight=2.0)

declare the assembly as a list of constraint objects::

    constraints = [
        RequiredSkill(),                     # hard, weight 1
        MinimumRest(0.25, hard=False),       # demoted to a cheap soft cost
        UndesiredDay(2.0),                   # soft, double weight
        BalanceShiftAssignments(exponent=2.0),
    ]
    solver.solve(schedule, DecomposedCalculator(constraints))

Every constraint takes ``(weight=1.0, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint's cost.
- ``hard``     ``True``/``False`` overrides the constraint's DEFAULT level
               (shown in each class docstring); ``None`` keeps the default.
- ``exponent`` sign-preserving power shaping of the per-employee cost:
               ``weight * sign(c) * |c| ** exponent``. 2.0 = squared —
               concentrated violations become disproportionately expensive.

These are plain data holders; the C++ core reads ``.name`` / ``.weight`` /
``.hard`` / ``.exponent`` when a list is passed to ``DecomposedCalculator``.
"""

from dataclasses import dataclass, fields
from typing import ClassVar

__all__ = [
    "Constraint",
    "RequiredSkill",
    "UnavailableEmployee",
    "OverlappingShift",
    "MaxOneShiftPerDay",
    "MinimumRest",
    "UndesiredDay",
    "DesiredDay",
    "DesiredShift",
    "BalanceShiftAssignments",
    "MaxConsecutiveDays",
    "MinConsecutiveDays",
    "MaxConsecutiveOff",
    "RestAfterConsecutiveWork",
    "MaxWeeklyMinutes",
    "MinWeeklyMinutes",
    "MinWeeklyRestDays",
    "AssignmentCost",
    "UnassignedShift",
    "BalanceMinMaxGap",
    "BalanceWeeklyMinutesGap",
    "get_constraints",
]

_BASE_FIELDS = ("weight", "hard", "exponent")


@dataclass(frozen=True)
class Constraint:
    """Base spec: a constraint pick with weight / level / shaping."""

    weight: float = 1.0
    hard: bool | None = None  # None = keep the constraint's default level
    exponent: float = 1.0
    name: ClassVar[str] = ""

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} must set a registry `name`")

    @property
    def params(self) -> dict:
        """Constraint-specific options (every dataclass field beyond
        weight/hard/exponent) — read by the C++ core."""
        return {f.name: getattr(self, f.name) for f in fields(self) if f.name not in _BASE_FIELDS}


class RequiredSkill(Constraint):
    """1 per assigned shift whose required skill the employee lacks.
    Default level: hard."""

    name = "required_skill"


class UnavailableEmployee(Constraint):
    """Minutes of assigned shifts overlapping the employee's unavailable days.
    Default level: hard."""

    name = "unavailable_employee"


class OverlappingShift(Constraint):
    """Overlap minutes between two shifts of the same employee.
    Default level: hard."""

    name = "overlapping_shift"


class MaxOneShiftPerDay(Constraint):
    """1 per pair of same-employee shifts starting on the same day.
    Default level: hard."""

    name = "one_shift_per_day"


@dataclass(frozen=True)
class MinimumRest(Constraint):
    """Rest shortfall below `min_gap_minutes` (default 10 hours) between two
    same-employee shifts. Default level: hard."""

    min_gap_minutes: int = 600
    name: ClassVar[str] = "min_rest"


class UndesiredDay(Constraint):
    """Minutes of assigned shifts on the employee's undesired days.
    Default level: soft."""

    name = "undesired_day"


class DesiredDay(Constraint):
    """Reward: minutes on the employee's desired days (negative cost).
    Default level: soft."""

    name = "desired_day"


class DesiredShift(Constraint):
    """Reward: location-specific work preference — rewards only when a worker
    is assigned a shift whose (day, location) is in their ``desired_shifts`` (the
    requested location must match, not just the day). Default level: soft."""

    name = "desired_shift"


class BalanceShiftAssignments(Constraint):
    """Load-balancing "unfairness" over per-employee shift counts (global
    statistic, not per-employee). Default level: soft."""

    name = "balance_assignments"


@dataclass(frozen=True)
class MaxConsecutiveDays(Constraint):
    """Days beyond `value` in any run of consecutive worked days — e.g.
    value=6 means "at most 6 days in a row". A day counts once even with
    several shifts. Default level: hard."""

    value: int = 6
    name: ClassVar[str] = "max_consecutive_days"


@dataclass(frozen=True)
class MinConsecutiveDays(Constraint):
    """Shortfall below `value` of any run of consecutive worked days — "if
    you work, work at least `value` days in a row". Default level: soft."""

    value: int = 2
    name: ClassVar[str] = "min_consecutive_days"


@dataclass(frozen=True)
class MaxConsecutiveOff(Constraint):
    """Interior runs of more than `value` consecutive days off (bounded by work
    on both sides; off days at the horizon edge are not counted). value=3
    penalizes runs of 4+ days off. Default level: soft."""

    value: int = 3
    name: ClassVar[str] = "max_consecutive_off"


@dataclass(frozen=True)
class RestAfterConsecutiveWork(Constraint):
    """After a run of >= `work_days` consecutive worked days, the employee must
    get >= `rest_days` consecutive rest days before working again; penalty is
    the shortfall. A run at the end of the horizon is not penalized. With
    MaxConsecutiveDays(work_days) this enforces a work_days-on / rest_days-off
    pattern. Default level: hard."""

    work_days: int = 6
    rest_days: int = 3
    name: ClassVar[str] = "rest_after_consecutive_work"


@dataclass(frozen=True)
class MaxWeeklyMinutes(Constraint):
    """Assigned minutes beyond `minutes` in any calendar week (7-day blocks
    from day 0; a shift counts fully in its start week). 2400 = a 40-hour
    week. Default level: hard."""

    minutes: int = 40 * 60
    name: ClassVar[str] = "max_weekly_minutes"


@dataclass(frozen=True)
class MinWeeklyMinutes(Constraint):
    """Shortfall below `minutes` in EVERY calendar week of the horizon
    (guaranteed-hours semantics: an idle week costs the full threshold).
    Default level: soft."""

    minutes: int = 8 * 60
    name: ClassVar[str] = "min_weekly_minutes"


@dataclass(frozen=True)
class MinWeeklyRestDays(Constraint):
    """Worked days beyond ``7 - value`` in any calendar week (7-day blocks from
    day 0) — i.e. "at least `value` rest days per week". A day counts once even
    with several shifts. value=1 means "at most 6 worked days per week".
    Default level: hard."""

    value: int = 1
    name: ClassVar[str] = "min_weekly_rest_days"


@dataclass(frozen=True)
class AssignmentCost(Constraint):
    """Monetary cost of the assignment. Per (employee, shift): an explicit
    `shift_costs[employee_name][shift_id]` entry if present, otherwise
    `cost_per_hour` * duration, otherwise `cost_per_shift`.
    Default level: soft."""

    cost_per_hour: float = 0.0
    cost_per_shift: float = 0.0
    shift_costs: dict | None = None  # {employee_name: {shift_id: cost}}
    name: ClassVar[str] = "assignment_cost"

    @property
    def params(self) -> dict:
        p = super().params
        if p["shift_costs"] is None:
            p["shift_costs"] = {}
        return p


class UnassignedShift(Constraint):
    """1 per shift with no employee assigned (coverage guarantee).
    Default level: hard."""

    name = "unassigned_shift"


@dataclass(frozen=True)
class BalanceMinMaxGap(Constraint):
    """Fairness as the min-max gap: how far the busiest employee's shift
    count is above the least busy one's, beyond `tolerance`.
    Default level: soft."""

    tolerance: int = 0
    name: ClassVar[str] = "balance_min_max_gap"


@dataclass(frozen=True)
class BalanceWeeklyMinutesGap(Constraint):
    """Fairness of weekly hours: per calendar week (7-day blocks from day 0),
    the gap between the busiest and least-busy employee's assigned minutes
    beyond `tolerance_minutes`, summed over weeks. The cost is in HOURS (excess
    minutes / 60) to stay comparable to the other soft terms. All employees are
    compared, so one who works zero minutes in a week counts as 0. Default
    level: soft."""

    tolerance_minutes: int = 120
    name: ClassVar[str] = "balance_weekly_minutes_gap"


#: The default assembly, in the order `es::defaultConstraints()` builds it.
#: Eight of the specs above — the rest are opt-in extras, which is why
#: `CONSTRAINT_NAMES` is longer than this list.
_DEFAULTS: tuple[type[Constraint], ...] = (
    RequiredSkill,
    UnavailableEmployee,
    OverlappingShift,
    MaxOneShiftPerDay,
    MinimumRest,
    UndesiredDay,
    DesiredDay,
    BalanceShiftAssignments,
)


def get_constraints(
    constraints: list[Constraint] | None = None,
) -> list[Constraint]:
    """The cost function, as a list of specs. No argument = the defaults.

    The same entry point every use case in this repo exposes, so "start from
    the defaults and change one thing" reads the same everywhere::

        from es_scheduling.constraints import UndesiredDay, get_constraints

        solver.solve(schedule, DecomposedCalculator(get_constraints()))
        solver.solve(schedule, DecomposedCalculator(get_constraints([
            RequiredSkill(), UndesiredDay(2.0),
        ])))

    Passing `constraints` uses that list as given — only what is in it is
    scored. The specs themselves are unchanged; this is a shorthand for the
    default eight, not a new way to declare them.
    """
    if constraints is None:
        return [cls() for cls in _DEFAULTS]
    if not constraints:
        # Same contract as the C++ applyConstraints: an empty list means "score
        # nothing", which is almost certainly a mistake. A filter that happens
        # to come back empty must not silently flip to "score everything".
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return list(constraints)
