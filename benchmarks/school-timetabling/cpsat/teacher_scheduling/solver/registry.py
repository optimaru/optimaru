"""Constraint catalog — a plugin registry with one class per constraint.

A class's fields are the constraint's parameters and `name` is its API
identifier. `solve()` takes a single list of these objects — no separate hard
and soft lists; each constraint decides itself whether to block with
`model.add()` (hard) or return a penalty expression (soft). When `build()`
returns None it acted as a hard constraint only; an expression it returns
becomes the penalty term named `name`.

    from teacher_scheduling.solver.registry import WeeklyPeriodCounts, default_constraints, merge

    # Turn unfillable periods into a very expensive penalty instead of INFEASIBLE (= solve_partial)
    timetable = solve(
        problem,
        config,
        constraints=merge(default_constraints(), WeeklyPeriodCounts(hard=False, weight=10_000)),
    )

Weights come from the constraint instance's `weight` field. A custom
constraint without `weight` is looked up with `ObjectiveWeights.weight_for(name)`,
and defaults to 1 if not found there either.

The catalog (`CONSTRAINT_TYPES` / `constraint_specs()`) is the single source a
caller reads to learn "which constraints can be switched on, with which
parameters". A separately maintained list of names would drift from the
solver, so read it from here rather than copying it by hand.

Note: `validate.validate_timetable` / `compute_penalties` check against the
default configuration (hard H1–H10, soft S1–S6) only. Whether a solution
obtained with relaxed or injected custom constraints respects those rules is
for the caller to verify.
"""

from collections.abc import Iterable, Sequence
from dataclasses import dataclass, fields
from typing import Any, ClassVar, Protocol, runtime_checkable

from ortools.sat.python import cp_model

from teacher_scheduling.solver import constraints as hard_impl
from teacher_scheduling.solver import objective
from teacher_scheduling.solver.config import ObjectiveWeights
from teacher_scheduling.solver.variables import TimetableVariables

#: Default penalty per unassigned period. Clearly larger than the sum of the other soft terms.
UNASSIGNED_WEIGHT = 10_000


@runtime_checkable
class Constraint(Protocol):
    """A constraint plugin.

    `build()` adds hard constraints to the model and, if there is something to
    count as a penalty, returns that linear expression. The returned expression
    is reported as `Timetable.penalties[name]` and added to the objective with
    weight `weight`.
    """

    name: str

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None: ...


# ---------------------------------------------------------------- hard constraints (H1–H10)


@dataclass(frozen=True)
class WeeklyPeriodCounts:
    """H1: weekly periods per course.

    `hard=True` matches them exactly; `hard=False` relaxes to "at most" and
    turns unfilled periods into a penalty — Comcigan's "unassigned list" works
    this way. With the relaxed mode's `weight` above the sum of the other soft
    terms, every period that can be filled is filled and only the truly
    impossible ones stay unassigned.
    """

    name: ClassVar[str] = "weekly_period_counts"
    label: ClassVar[str] = "Weekly periods met"

    hard: bool = True
    weight: int = UNASSIGNED_WEIGHT

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        if self.hard:
            hard_impl._add_weekly_period_counts(model, variables)
            return None

        for course in variables.problem.courses:
            model.add(sum(variables.course_vars(course.id)) <= course.periods_per_week)
        # A parallel group counts only its representative course. H5 ties the whole
        # group to the same times, so its unassigned periods are always equal; counting
        # every member would charge one missing cell group-size times and unfairly
        # favour parallel groups over ordinary courses.
        terms = [
            course.periods_per_week - sum(variables.course_vars(course.id))
            for course in variables.occupancy_representatives()
        ]
        return sum(terms) if terms else None


@dataclass(frozen=True)
class _HardOnly:
    """Common body of hard constraints that are only switched on or off, without parameters."""

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> None:
        """Returns no penalty expression (None), so it acts as a hard constraint only."""
        self._impl(model, variables)

    @staticmethod
    def _impl(model: cp_model.CpModel, variables: TimetableVariables) -> None:  # pragma: no cover - set by subclasses
        raise NotImplementedError


@dataclass(frozen=True)
class FixedSlots(_HardOnly):
    """H2: fixed-time placement."""

    name: ClassVar[str] = "fixed_slots"
    label: ClassVar[str] = "Fixed placements / forbidden times"
    _impl = staticmethod(hard_impl._add_fixed_slots)


@dataclass(frozen=True)
class ParallelGroups(_HardOnly):
    """H5: parallel sections run together."""

    name: ClassVar[str] = "parallel_groups"
    label: ClassVar[str] = "Parallel sections together"
    _impl = staticmethod(hard_impl._add_parallel_groups)


@dataclass(frozen=True)
class ClassGroupConflicts(_HardOnly):
    """H3: no class clashes."""

    name: ClassVar[str] = "class_group_conflicts"
    label: ClassVar[str] = "No double-booked class"
    _impl = staticmethod(hard_impl._add_class_group_conflicts)


@dataclass(frozen=True)
class TeacherConflicts(_HardOnly):
    """H4: no teacher clashes."""

    name: ClassVar[str] = "teacher_conflicts"
    label: ClassVar[str] = "No double-booked teacher"
    _impl = staticmethod(hard_impl._add_teacher_conflicts)


@dataclass(frozen=True)
class RoomCapacity(_HardOnly):
    """H6: special-room capacity."""

    name: ClassVar[str] = "room_capacity"
    label: ClassVar[str] = "Special-room capacity"
    _impl = staticmethod(hard_impl._add_room_capacity)


@dataclass(frozen=True)
class DailyCourseLimits(_HardOnly):
    """H7: daily maximum periods per course."""

    name: ClassVar[str] = "daily_course_limits"
    label: ClassVar[str] = "Daily periods per course"
    _impl = staticmethod(hard_impl._add_daily_course_limits)


@dataclass(frozen=True)
class BlockPeriods(_HardOnly):
    """H8: block time (consecutive periods)."""

    name: ClassVar[str] = "block_periods"
    label: ClassVar[str] = "Consecutive blocks"
    _impl = staticmethod(hard_impl._add_block_periods)


@dataclass(frozen=True)
class TeacherWorkload(_HardOnly):
    """H9: daily/weekly maximum periods per teacher."""

    name: ClassVar[str] = "teacher_workload"
    label: ClassVar[str] = "Teacher period caps"
    _impl = staticmethod(hard_impl._add_teacher_workload)


@dataclass(frozen=True)
class TeacherConsecutiveLimits(_HardOnly):
    """H10: teacher consecutive-lessons limit."""

    name: ClassVar[str] = "teacher_consecutive_limits"
    label: ClassVar[str] = "Teacher consecutive lessons"
    _impl = staticmethod(hard_impl._add_teacher_consecutive_limits)


# ---------------------------------------------------------------- soft terms (S1–S6)


@dataclass(frozen=True)
class TeacherGap:
    """S1: idle periods in the middle of a teacher's day."""

    name: ClassVar[str] = "teacher_gap"
    label: ClassVar[str] = "Minimise teacher gaps"

    weight: int = 20

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none([span.gap for span in variables.teacher_day_spans().values()])


@dataclass(frozen=True)
class TeacherWorkday:
    """S2: number of teacher working days."""

    name: ClassVar[str] = "teacher_workday"
    label: ClassVar[str] = "Minimise teacher working days"

    weight: int = 3

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none([span.working for span in variables.teacher_day_spans().values()])


@dataclass(frozen=True)
class TeacherFreeDay:
    """S3: lessons placed on a requested free day."""

    name: ClassVar[str] = "teacher_free_day"
    label: ClassVar[str] = "Teacher requested free days"

    weight: int = 30

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        spans = variables.teacher_day_spans()
        return objective._sum_or_none(objective._free_day_violations(variables, spans))


@dataclass(frozen=True)
class ClassGap:
    """S4: idle periods in the middle of a class's day."""

    name: ClassVar[str] = "class_gap"
    label: ClassVar[str] = "Minimise class gaps"

    weight: int = 10

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none([span.gap for span in variables.class_day_spans().values()])


@dataclass(frozen=True)
class AvoidedPeriod:
    """S5: lessons placed in a subject's avoided periods."""

    name: ClassVar[str] = "avoided_period"
    label: ClassVar[str] = "Avoid avoided periods"

    weight: int = 5

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none(objective._period_preference_vars(variables, avoided=True))


@dataclass(frozen=True)
class NonPreferredPeriod:
    """S6: lessons placed outside a subject's preferred periods."""

    name: ClassVar[str] = "non_preferred_period"
    label: ClassVar[str] = "Favour preferred periods"

    weight: int = 1

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none(objective._period_preference_vars(variables, avoided=False))


# ---------------------------------------------------------------- catalog

#: Every registered constraint. The order is the default assembly order (hard H1–H10 -> soft S1–S6).
#: A class added here appears automatically in the DTOs (`teacher_scheduling.api`)
#: and in the catalog.
CONSTRAINT_TYPES: tuple[type[Constraint], ...] = (
    WeeklyPeriodCounts,  # H1
    FixedSlots,  # H2
    ParallelGroups,  # H5
    ClassGroupConflicts,  # H3
    TeacherConflicts,  # H4
    RoomCapacity,  # H6
    DailyCourseLimits,  # H7
    BlockPeriods,  # H8
    TeacherWorkload,  # H9
    TeacherConsecutiveLimits,  # H10
    TeacherGap,  # S1
    TeacherWorkday,  # S2
    TeacherFreeDay,  # S3
    ClassGap,  # S4
    AvoidedPeriod,  # S5
    NonPreferredPeriod,  # S6
)

CONSTRAINT_TYPE_BY_NAME: dict[str, type[Constraint]] = {c.name: c for c in CONSTRAINT_TYPES}


@dataclass(frozen=True)
class ConstraintParam:
    """One parameter a constraint takes (= one dataclass field)."""

    name: str
    type_name: str
    default: Any


@dataclass(frozen=True)
class ConstraintSpec:
    """One catalog row: what can be switched on, and how."""

    name: str
    label: str
    #: `"hard"` cannot be switched off or relaxed, `"soft"` is a weighted penalty only,
    #: `"either"` implements both modes through its `hard` parameter.
    kind: str
    params: tuple[ConstraintParam, ...]

    @property
    def default_weight(self) -> int:
        for param in self.params:
            if param.name == "weight":
                return param.default
        return 0


def _params_of(constraint_type: type[Constraint]) -> tuple[ConstraintParam, ...]:
    return tuple(
        ConstraintParam(name=f.name, type_name=getattr(f.type, "__name__", str(f.type)), default=f.default)
        for f in fields(constraint_type)
    )


def constraint_specs() -> tuple[ConstraintSpec, ...]:
    """Names/labels/parameters of the registered constraints. Read this instead of a hand-copied list."""
    specs = []
    for constraint_type in CONSTRAINT_TYPES:
        params = _params_of(constraint_type)
        names = {param.name for param in params}
        if "hard" in names:
            kind = "either"
        elif "weight" in names:
            kind = "soft"
        else:
            kind = "hard"
        specs.append(ConstraintSpec(name=constraint_type.name, label=constraint_type.label, kind=kind, params=params))
    return tuple(specs)


def default_constraints(weights: ObjectiveWeights | None = None) -> tuple[Constraint, ...]:
    """Assembles the default configuration (hard H1–H10 + soft S1–S6) with default parameters.

    Given `weights`, overrides `weight` only for the names **actually present
    there** — so that existing calls passing weights via `ObjectiveWeights` keep
    working. `weight_for`'s default of 1 must not leak in for unknown names,
    hence the presence check.
    """
    given = weights.model_dump() if weights is not None else {}
    built = []
    for constraint_type in CONSTRAINT_TYPES:
        kwargs: dict[str, Any] = {}
        name = constraint_type.name
        if name in given and any(param.name == "weight" for param in _params_of(constraint_type)):
            kwargs["weight"] = given[name]
        built.append(constraint_type(**kwargs))
    return tuple(built)


def merge(base: Sequence[Constraint], *overrides: Constraint) -> tuple[Constraint, ...]:
    """Replaces same-named entries of `base` with `overrides` and appends new names.

    Keeps positions so the assembly order (= variable creation order) does not change.
    """
    replacement = {constraint.name: constraint for constraint in overrides}
    merged = [replacement.pop(constraint.name, constraint) for constraint in base]
    merged.extend(constraint for constraint in overrides if constraint.name in replacement)
    return tuple(merged)


def weight_of(constraint: Constraint, weights: ObjectiveWeights) -> int:
    """A constraint's weight. The instance's `weight` is the source; otherwise looked up in `weights`."""
    own = getattr(constraint, "weight", None)
    return weights.weight_for(constraint.name) if own is None else own


def build_all(
    model: cp_model.CpModel,
    variables: TimetableVariables,
    constraints: Iterable[Constraint],
) -> objective.PenaltyTerms:
    """Assembles the constraints in order and collects, by name, the ones that returned a penalty expression."""
    penalties: objective.PenaltyTerms = {}
    for constraint in constraints:
        expr = constraint.build(model, variables)
        if expr is not None:
            penalties[constraint.name] = expr
    return penalties


__all__ = [
    "CONSTRAINT_TYPES",
    "CONSTRAINT_TYPE_BY_NAME",
    "UNASSIGNED_WEIGHT",
    "AvoidedPeriod",
    "BlockPeriods",
    "ClassGap",
    "ClassGroupConflicts",
    "Constraint",
    "ConstraintParam",
    "ConstraintSpec",
    "DailyCourseLimits",
    "FixedSlots",
    "NonPreferredPeriod",
    "ParallelGroups",
    "RoomCapacity",
    "TeacherConflicts",
    "TeacherConsecutiveLimits",
    "TeacherFreeDay",
    "TeacherGap",
    "TeacherWorkday",
    "TeacherWorkload",
    "WeeklyPeriodCounts",
    "build_all",
    "constraint_specs",
    "default_constraints",
    "merge",
    "weight_of",
]
