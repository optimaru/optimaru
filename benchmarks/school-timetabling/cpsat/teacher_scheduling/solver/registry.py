"""Constraint catalog — a plugin registry with one class per constraint.

Each built-in constraint is a subclass of the engine's spec of the same
constraint (`st_timetabling.constraints`): one name per constraint across the
engine, this CP-SAT reference, the server and the timetable penalties
(`unassigned_lessons_cost`, `class_conflict_cost`, ... `parallel_groups_cost`),
and the same three knobs, ``(weight=None, level=None, exponent=1.0)``. The
defaults (level, weight, flippability) are the engine's — read from
`st_timetabling.constraint_info()`, the C++ components' kDefaultLevel /
kDefaultWeight — never restated here.

`solve()` takes a single list of these objects — no separate hard and soft
lists; each constraint decides itself whether to block with `model.add()`
(hard) or return a penalty expression (soft). When `build()` returns None it
acted as a hard constraint only; an expression it returns becomes the penalty
term named `name`.

    from teacher_scheduling.solver.registry import UnassignedLessonsCost, default_constraints, merge

    # Turn unfillable periods into a very expensive penalty instead of INFEASIBLE (= solve_partial)
    timetable = solve(
        problem,
        config,
        constraints=merge(default_constraints(), UnassignedLessonsCost(level="soft", weight=10_000)),
    )

The knobs on the CP-SAT path:

- ``weight``   ``None`` = the engine default; otherwise a non-negative integer
               (CP-SAT objectives are integral). On a hard constraint the
               weight only switches it: 0 leaves it out, anything else
               enforces it.
- ``level``    ``None`` = the default level. ``"hard"`` on a soft constraint
               forbids every violation (``model.add(expr == 0)``); ``"soft"``
               on ``unassigned_lessons_cost`` is partial timetabling (at most
               the weekly periods, the shortfall a penalty). The other hard
               constraints are model constraints here and refuse ``"soft"``;
               ``fixed_slots_cost`` / ``parallel_groups_cost`` are never
               flippable (the engine refuses it too).
- ``exponent`` only 1.0 (a documented CP-SAT limitation: the objective is
               linear).

A custom constraint without `weight` is looked up with
`ObjectiveWeights.weight_for(name)`, and defaults to 1 if not found there
either.

The catalog (`CONSTRAINT_TYPES` / `constraint_specs()`) is the single source a
caller reads to learn "which constraints can be switched on, with which
parameters". A separately maintained list of names would drift from the
solver, so read it from here rather than copying it by hand.

Note: `validate.validate_timetable` / `compute_penalties` check against the
default configuration only. Whether a solution obtained with relaxed or
injected custom constraints respects those rules is for the caller to verify.
"""

from collections.abc import Iterable, Sequence
from dataclasses import dataclass, fields
from typing import Any, ClassVar, Protocol, runtime_checkable

from ortools.sat.python import cp_model
from st_timetabling import constraint_info as _engine_constraint_info
from st_timetabling import constraints as domain

from teacher_scheduling.solver import constraints as hard_impl
from teacher_scheduling.solver import objective
from teacher_scheduling.solver.config import ObjectiveWeights
from teacher_scheduling.solver.variables import TimetableVariables

#: Penalty per unassigned period in partial timetabling: the engine's documented soft weight of
#: unassigned_lessons_cost (C++ UnassignedLessonsCost::kSoftWeight). Clearly larger than the sum
#: of the other soft terms.
UNASSIGNED_WEIGHT = int(domain.UNASSIGNED_LESSONS_SOFT_WEIGHT)

#: The engine's catalog rows, by name: the default level / weight / flippability of each constraint.
ENGINE_DEFAULTS: dict[str, Any] = {row.name: row for row in _engine_constraint_info()}


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


class _EnginePlugin:
    """The CP-SAT side of one engine constraint spec: the knob rules and `build()`.

    Mixed into a subclass of the matching `st_timetabling.constraints` spec, so
    the name, the labels and the knob fields (weight / level / exponent) are the
    spec's own.
    """

    name: ClassVar[str]
    weight: float | None
    level: str | None
    exponent: float

    @property
    def default_level(self) -> str:
        return ENGINE_DEFAULTS[self.name].default_level

    @property
    def effective_level(self) -> str:
        """The level the knobs ask for, with the engine's rules (level=None keeps the default)."""
        row = ENGINE_DEFAULTS[self.name]
        if self.level is None:
            return row.default_level
        wanted = str(self.level)
        if wanted == "medium":
            raise ValueError(f"constraint {self.name!r}: this use case has no medium level")
        if wanted == "hard" and row.reward:
            raise ValueError(f"constraint {self.name!r} is a reward (raw cost <= 0) and cannot be hard")
        if wanted != row.default_level and not row.level_flippable:
            raise ValueError(
                f"constraint {self.name!r} is always {row.default_level}; level={wanted!r} is not allowed"
            )
        return wanted

    @property
    def effective_weight(self) -> int:
        """The weight in effect: the knob, or the engine default when it is None. Integral on CP-SAT."""
        weight = ENGINE_DEFAULTS[self.name].default_weight if self.weight is None else self.weight
        if float(weight) != int(weight):
            raise ValueError(f"constraint {self.name!r}: CP-SAT needs an integer weight, not {weight!r}")
        return int(weight)

    def build(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        if self.exponent != 1.0:
            raise ValueError(f"constraint {self.name!r}: exponent {self.exponent!r} is not supported by CP-SAT")
        level, weight = self.effective_level, self.effective_weight
        if level == "hard":
            if weight == 0:
                return None  # a zero weight switches the constraint off
            return self._build_hard(model, variables)
        return self._build_soft(model, variables)

    def _build_hard(self, model: cp_model.CpModel, variables: TimetableVariables) -> None:  # pragma: no cover
        raise NotImplementedError

    def _build_soft(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        raise ValueError(
            f"constraint {self.name!r}: the CP-SAT model enforces it structurally, so level='soft' is not supported"
        )


class _Structural(_EnginePlugin):
    """A hard constraint the CP-SAT model states directly (`model.add`), without a penalty form."""

    def _build_hard(self, model: cp_model.CpModel, variables: TimetableVariables) -> None:
        self._impl(model, variables)

    @staticmethod
    def _impl(model: cp_model.CpModel, variables: TimetableVariables) -> None:  # pragma: no cover - set by subclasses
        raise NotImplementedError


class _Penalty(_EnginePlugin):
    """A soft constraint: a penalty expression, or with level="hard" a constraint that it is 0."""

    def _build_hard(self, model: cp_model.CpModel, variables: TimetableVariables) -> None:
        expr = self._expression(model, variables)
        if expr is not None:
            model.add(expr == 0)

    def _build_soft(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return self._expression(model, variables)

    def _expression(
        self, model: cp_model.CpModel, variables: TimetableVariables
    ) -> cp_model.LinearExpr | None:  # pragma: no cover
        raise NotImplementedError


# ---------------------------------------------------------------- hard constraints


class UnassignedLessonsCost(domain.UnassignedLessonsCost, _EnginePlugin):
    """Weekly periods per course.

    Hard (the default) matches them exactly; ``level="soft"`` relaxes to "at
    most" and turns unfilled periods into a penalty — Comcigan's "unassigned
    list" works this way. With the relaxed mode's `weight` above the sum of the
    other soft terms (`UNASSIGNED_WEIGHT`), every period that can be filled is
    filled and only the truly impossible ones stay unassigned.
    """

    def _build_hard(self, model: cp_model.CpModel, variables: TimetableVariables) -> None:
        hard_impl._add_unassigned_lessons_cost(model, variables)

    def _build_soft(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        for course in variables.problem.courses:
            model.add(sum(variables.course_vars(course.id)) <= course.periods_per_week)
        # A parallel group counts only its representative course. The parallel-groups rule ties
        # the whole group to the same times, so its unassigned periods are always equal; counting
        # every member would charge one missing cell group-size times and unfairly favour parallel
        # groups over ordinary courses (the engine's merged lesson is one lesson too).
        terms = [
            course.periods_per_week - sum(variables.course_vars(course.id))
            for course in variables.occupancy_representatives()
        ]
        return sum(terms) if terms else None


class FixedSlotsCost(domain.FixedSlotsCost, _Structural):
    """Fixed-time placement (never flippable)."""

    _impl = staticmethod(hard_impl._add_fixed_slots_cost)


class ParallelGroupsCost(domain.ParallelGroupsCost, _Structural):
    """Parallel sections run together (never flippable)."""

    _impl = staticmethod(hard_impl._add_parallel_groups_cost)


class ClassConflictCost(domain.ClassConflictCost, _Structural):
    """No class clashes."""

    _impl = staticmethod(hard_impl._add_class_conflict_cost)


class TeacherConflictCost(domain.TeacherConflictCost, _Structural):
    """No teacher clashes."""

    _impl = staticmethod(hard_impl._add_teacher_conflict_cost)


class DisallowedSlotCost(domain.DisallowedSlotCost, _Structural):
    """Lessons only on allowed times: structural here — CP-SAT never creates a variable for a forbidden
    time or an unavailable teacher/class (variables.py), so there is nothing to add."""

    @staticmethod
    def _impl(model: cp_model.CpModel, variables: TimetableVariables) -> None:
        return None


class RoomCapacityCost(domain.RoomCapacityCost, _Structural):
    """Special-room capacity."""

    _impl = staticmethod(hard_impl._add_room_capacity_cost)


class CourseMaxPeriodsPerDayCost(domain.CourseMaxPeriodsPerDayCost, _Structural):
    """Daily maximum periods per course."""

    _impl = staticmethod(hard_impl._add_course_max_periods_per_day_cost)


class BlockAdjacencyCost(domain.BlockAdjacencyCost, _Structural):
    """Block time (consecutive periods)."""

    _impl = staticmethod(hard_impl._add_block_adjacency_cost)


class TeacherMaxPeriodsCost(domain.TeacherMaxPeriodsCost, _Structural):
    """Daily/weekly maximum periods per teacher."""

    _impl = staticmethod(hard_impl._add_teacher_max_periods_cost)


class TeacherMaxConsecutivePeriodsCost(domain.TeacherMaxConsecutivePeriodsCost, _Structural):
    """Teacher consecutive-lessons limit."""

    _impl = staticmethod(hard_impl._add_teacher_max_consecutive_periods_cost)


# ---------------------------------------------------------------- soft constraints


class TeacherGapCost(domain.TeacherGapCost, _Penalty):
    """Idle periods in the middle of a teacher's day."""

    def _expression(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none([span.gap for span in variables.teacher_day_spans().values()])


class TeacherWorkdayCost(domain.TeacherWorkdayCost, _Penalty):
    """Number of teacher working days."""

    def _expression(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none([span.working for span in variables.teacher_day_spans().values()])


class TeacherFreeDayCost(domain.TeacherFreeDayCost, _Penalty):
    """Worked requested free days: a teacher's working day on one of their preferred free days."""

    def _expression(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        spans = variables.teacher_day_spans()
        return objective._sum_or_none(objective._free_day_violations(variables, spans))


class ClassGapCost(domain.ClassGapCost, _Penalty):
    """Idle periods in the middle of a class's day."""

    def _expression(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none([span.gap for span in variables.class_day_spans().values()])


class AvoidedPeriodCost(domain.AvoidedPeriodCost, _Penalty):
    """Lessons placed in a subject's avoided periods."""

    def _expression(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none(objective._period_preference_vars(variables, avoided=True))


class NonPreferredPeriodCost(domain.NonPreferredPeriodCost, _Penalty):
    """Lessons placed outside a subject's preferred periods."""

    def _expression(self, model: cp_model.CpModel, variables: TimetableVariables) -> cp_model.LinearExpr | None:
        return objective._sum_or_none(objective._period_preference_vars(variables, avoided=False))


# ---------------------------------------------------------------- catalog

#: Every registered constraint, in the engine's list_constraints() order (the default assembly
#: order). A class added here appears automatically in the DTOs (`teacher_scheduling.api`) and in
#: the catalog.
CONSTRAINT_TYPES: tuple[type[Constraint], ...] = (
    UnassignedLessonsCost,
    ClassConflictCost,
    TeacherConflictCost,
    DisallowedSlotCost,
    RoomCapacityCost,
    CourseMaxPeriodsPerDayCost,
    BlockAdjacencyCost,
    TeacherMaxPeriodsCost,
    TeacherMaxConsecutivePeriodsCost,
    TeacherGapCost,
    TeacherWorkdayCost,
    TeacherFreeDayCost,
    ClassGapCost,
    AvoidedPeriodCost,
    NonPreferredPeriodCost,
    FixedSlotsCost,
    ParallelGroupsCost,
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
    """One catalog row: what can be switched on, and how — with the engine's defaults."""

    name: str
    label: str
    #: `"either"` when a selection may move the constraint, else its fixed level (the
    #: engine's `level_flippable`).
    kind: str
    params: tuple[ConstraintParam, ...]
    default_level: str = "hard"
    default_weight: int | float = 1
    level_flippable: bool = True
    reward: bool = False
    default_enabled: bool = True


def _params_of(constraint_type: type[Constraint]) -> tuple[ConstraintParam, ...]:
    return tuple(
        ConstraintParam(name=f.name, type_name=getattr(f.type, "__name__", str(f.type)), default=f.default)
        for f in fields(constraint_type)
    )


def constraint_specs() -> tuple[ConstraintSpec, ...]:
    """Names/labels/parameters/defaults of the registered constraints, from the engine's catalog.

    Read this instead of a hand-copied list."""
    specs = []
    for constraint_type in CONSTRAINT_TYPES:
        row = ENGINE_DEFAULTS[constraint_type.name]
        weight = row.default_weight
        specs.append(
            ConstraintSpec(
                name=constraint_type.name,
                label=constraint_type.label_en,
                kind="either" if row.level_flippable else row.default_level,
                params=_params_of(constraint_type),
                default_level=row.default_level,
                default_weight=int(weight) if float(weight).is_integer() else weight,
                level_flippable=row.level_flippable,
                reward=row.reward,
                default_enabled=row.default_enabled,
            )
        )
    return tuple(specs)


def default_constraints(weights: ObjectiveWeights | None = None) -> tuple[Constraint, ...]:
    """Assembles the default configuration with default parameters.

    Given `weights`, sets `weight` of the soft constraints whose names are
    **actually present there** — so that calls passing weights via
    `ObjectiveWeights` keep working (its extra fields name custom terms, and a
    hard constraint never takes its weight from there). `weight_for`'s default
    of 1 must not leak in for unknown names, hence the presence check.
    """
    given = weights.model_dump() if weights is not None else {}
    built = []
    for constraint_type in CONSTRAINT_TYPES:
        name = constraint_type.name
        if name in given and ENGINE_DEFAULTS[name].default_level == "soft":
            built.append(constraint_type(weight=given[name]))
        else:
            built.append(constraint_type())
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
    """A constraint's weight. A built-in constraint's knob (None = the engine default) is the
    source; a custom one's own `weight`, otherwise looked up in `weights`."""
    if isinstance(constraint, _EnginePlugin):
        return constraint.effective_weight
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
    "ENGINE_DEFAULTS",
    "UNASSIGNED_WEIGHT",
    "AvoidedPeriodCost",
    "BlockAdjacencyCost",
    "ClassConflictCost",
    "ClassGapCost",
    "Constraint",
    "ConstraintParam",
    "ConstraintSpec",
    "CourseMaxPeriodsPerDayCost",
    "DisallowedSlotCost",
    "FixedSlotsCost",
    "NonPreferredPeriodCost",
    "ParallelGroupsCost",
    "RoomCapacityCost",
    "TeacherConflictCost",
    "TeacherFreeDayCost",
    "TeacherGapCost",
    "TeacherMaxConsecutivePeriodsCost",
    "TeacherMaxPeriodsCost",
    "TeacherWorkdayCost",
    "UnassignedLessonsCost",
    "build_all",
    "constraint_specs",
    "default_constraints",
    "merge",
    "weight_of",
]
