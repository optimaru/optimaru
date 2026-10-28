"""Canonical constraint assembly specs — an API to build the cost function from plugins.

The default fast path (the monolithic incremental calculator) scores all of
H1–H10/S1–S6 in a hard-coded configuration. This module is the optional path
next to it: declare an assembly that picks constraints one by one, changes
their weights, flips hard/soft levels and drops some, then pass it to
``solve(problem, constraints=...)`` or ``canon.DecomposedCalculator``.

The shortest form is :class:`Assembly` — it starts from all 15 canonical
constraints, so "the defaults with a small change" is a one-liner::

    from st_timetabling.constraints import Assembly

    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap", weight=40).drop("teacher_workday"),
    )

An assembly can also be built from scratch as a list of spec instances::

    from st_timetabling.constraints import ClassConflict, TeacherConflict, TeacherGap

    constraints = [ClassConflict(), TeacherConflict(), TeacherGap(weight=40)]
    result = st.solve(problem, constraints=constraints)

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier of the constraint cost. ``None`` is the
               component default (hard H* = 1, soft S* = the cpsat
               ``ObjectiveWeights`` default — on the solve() path the
               ``weights`` argument replaces that default).
- ``hard``     ``True``/``False`` flips the default hard/soft level
               (see "Default: hard/soft" in each class docstring);
               ``None`` keeps the default.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. 2.0 = squared — concentrated violations get
               disproportionately expensive.

Specs are plain data holders; the C++ core reads ``.name`` / ``.weight`` /
``.hard`` / ``.exponent``. H2 (fixed time) has no spec — pinned lessons are
never moved by the engine and are not scored.
"""

from dataclasses import dataclass
from typing import ClassVar

from ._core import canon

__all__ = [
    "CPSAT_ALIASES",
    "ENGINE_ONLY",
    "STRUCTURAL_IN_ENGINE",
    "Assembly",
    "AvoidedPeriod",
    "BlockAdjacency",
    "ClassConflict",
    "ClassGap",
    "Constraint",
    "CourseDailyLimit",
    "NonPreferredPeriod",
    "RoomCapacity",
    "SlotNotAllowed",
    "TeacherConflict",
    "TeacherConsecutiveLimit",
    "TeacherFreeDay",
    "TeacherGap",
    "TeacherLoadLimit",
    "TeacherWorkday",
    "UnassignedLesson",
    "default_weights",
    "get_constraints",
    "soft_specs",
    "list_constraints",
    "resolve_name",
]


def list_constraints() -> tuple[str, ...]:
    """Every constraint name an assembly accepts (same order as the C++ registry)."""
    return tuple(canon.CONSTRAINT_NAMES)


@dataclass(frozen=True)
class Constraint:
    """Base spec: constraint choice + weight / level / shaping metadata."""

    weight: float | None = None  # None = keep the component's default weight
    hard: bool | None = None  # None = keep the component's default level
    exponent: float = 1.0
    name: ClassVar[str] = ""
    #: The component's default level/weight. These two are the source of the
    #: objective — a separately hand-kept list of soft weights would drift as
    #: soon as a constraint is added (`soft_specs()` / `default_weights()` derive from here).
    default_hard: ClassVar[bool] = True
    default_weight: ClassVar[float] = 1.0

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} must set the registry `name`")


class UnassignedLesson(Constraint):
    """H1: 1 point per unassigned lesson. Default: hard, weight 1.

    Demoting it with `hard=False` gives **partial timetabling** (the same
    contract as cpsat's `WeeklyPeriodCounts(hard=false)`): the search gains
    unassign moves, so "leave it out" becomes an option, and unfilled periods
    are reported as `penalties["weekly_period_counts"]` instead of a hard
    violation. `weight` is then the price of one unassigned period, so keep it
    above the sum of the other soft terms (cpsat default 10000) — set too low,
    lessons get dropped to save a few gaps."""

    name = "unassigned_lesson"


class ClassConflict(Constraint):
    """H3: 1 point per pair of same-class lessons at the same time. Default: hard, weight 1."""

    name = "class_conflict"


class TeacherConflict(Constraint):
    """H4: 1 point per teacher shared by a pair of lessons at the same time
    (team teaching included). Default: hard, weight 1."""

    name = "teacher_conflict"


class SlotNotAllowed(Constraint):
    """H5: 1 point per placement outside the allowed_slot table (forbidden
    times and teacher/class unavailability are folded into that table by the
    conversion). Default: hard, weight 1."""

    name = "slot_not_allowed"


class RoomCapacity(Constraint):
    """H6: sum over (special-room type, time) of max(0, demand - rooms).
    Default: hard, weight 1."""

    name = "room_capacity"


class CourseDailyLimit(Constraint):
    """H7: sum over (course, weekday) of max(0, placed - daily cap).
    Default: hard, weight 1."""

    name = "course_daily_limit"


class BlockAdjacency(Constraint):
    """H8: how far a block group's consecutive periods on one day are broken. Default: hard, weight 1."""

    name = "block_adjacency"


class TeacherLoadLimit(Constraint):
    """H9: sum of each teacher's excess over the daily/weekly period caps. Default: hard, weight 1."""

    name = "teacher_load_limit"


class TeacherConsecutiveLimit(Constraint):
    """H10: sum of each teacher's excess over the consecutive-lessons cap. Default: hard, weight 1."""

    name = "teacher_consecutive_limit"


class TeacherGap(Constraint):
    """S1: idle periods inside a teacher's day. Default: soft, weight 20."""

    name = "teacher_gap"
    default_hard = False
    default_weight = 20


class TeacherWorkday(Constraint):
    """S2: 1 point per (teacher, weekday) with lessons. Default: soft, weight 3."""

    name = "teacher_workday"
    default_hard = False
    default_weight = 3


class TeacherFreeDay(Constraint):
    """S3: 1 point per (teacher, weekday) with lessons on a requested free day.
    Default: soft, weight 30."""

    name = "teacher_free_day"
    default_hard = False
    default_weight = 30


class ClassGap(Constraint):
    """S4: idle periods inside a class's day. Default: soft, weight 10."""

    name = "class_gap"
    default_hard = False
    default_weight = 10


class AvoidedPeriod(Constraint):
    """S5: 1 point per lesson in an avoided period (parallel groups count per
    member subject). Default: soft, weight 5."""

    name = "avoided_period"
    default_hard = False
    default_weight = 5


class NonPreferredPeriod(Constraint):
    """S6: 1 point per lesson placed outside its preferred periods, when it has
    some. Default: soft, weight 1."""

    name = "non_preferred_period"
    default_hard = False
    default_weight = 1


_SPEC_CLASSES: tuple[type[Constraint], ...] = (
    UnassignedLesson,
    ClassConflict,
    TeacherConflict,
    SlotNotAllowed,
    RoomCapacity,
    CourseDailyLimit,
    BlockAdjacency,
    TeacherLoadLimit,
    TeacherConsecutiveLimit,
    TeacherGap,
    TeacherWorkday,
    TeacherFreeDay,
    ClassGap,
    AvoidedPeriod,
    NonPreferredPeriod,
)

_SPEC_BY_NAME: dict[str, type[Constraint]] = {cls.name: cls for cls in _SPEC_CLASSES}

#: cpsat (`teacher_scheduling.solver.registry`) name -> engine name.
#:
#: The two backends called the same constraints by different names. This
#: direction because cpsat is the reference for the constraint semantics (the
#: parity tests pin that). It lets the same assembly declaration go to both
#: backends unchanged — the premise of a performance comparison.
#: `room_capacity` and the six soft constraints already share names and are not listed.
CPSAT_ALIASES: dict[str, str] = {
    "weekly_period_counts": "unassigned_lesson",
    "class_group_conflicts": "class_conflict",
    "teacher_conflicts": "teacher_conflict",
    "daily_course_limits": "course_daily_limit",
    "block_periods": "block_adjacency",
    "teacher_workload": "teacher_load_limit",
    "teacher_consecutive_limits": "teacher_consecutive_limit",
}

#: Constraints in cpsat that the conversion step guarantees structurally in the engine.
#: Parallel sections are merged into one occupancy unit (convert.py) and fixed
#: placements are pinned Lessons the engine never moves. There is nothing to
#: switch off or tune, so they never enter an assembly.
STRUCTURAL_IN_ENGINE: frozenset[str] = frozenset({"fixed_slots", "parallel_groups"})

#: Conversely, specs that exist only in the engine. cpsat never creates
#: variables for impossible slots (variables.py), so it has nothing to score.
ENGINE_ONLY: frozenset[str] = frozenset({"slot_not_allowed"})


def soft_specs() -> tuple[type[Constraint], ...]:
    """Constraints that are soft by default (registry order) — the terms of the objective."""
    return tuple(cls for cls in _SPEC_CLASSES if not cls.default_hard)


def default_weights() -> dict[str, float]:
    """Soft term name -> default weight. Follows automatically when a constraint is added."""
    return {cls.name: cls.default_weight for cls in soft_specs()}


def resolve_name(name: str) -> str:
    """Maps a cpsat name to the engine name. Engine names pass through unchanged.

    Constraints the engine guarantees structurally (`STRUCTURAL_IN_ENGINE`)
    cannot be assembled, so they raise ValueError — the side bridging the two
    backends has to filter them.
    """
    if name in STRUCTURAL_IN_ENGINE:
        raise ValueError(
            f"{name!r} is guaranteed by the conversion step in the engine and cannot be assembled "
            "(parallel sections merge into one occupancy unit, fixed placements are pinned Lessons)"
        )
    return CPSAT_ALIASES.get(name, name)


class Assembly:
    """Canonical constraint assembly builder. It starts from all 15 — the same
    configuration as the monolithic default path — so "the defaults with a small
    change" stays short.

    ``add`` inserts a constraint by name (or spec instance) and **replaces one
    with the same name** (override semantics; for summed duplicates use
    ``canon.DecomposedCalculator.add`` directly). ``drop`` removes the named
    constraint. Both return self for chaining::

        Assembly().add("teacher_gap", weight=40).drop("teacher_workday")

    ``Assembly(constraints)`` with an iterable of spec instances/names starts
    from that list instead of the default set (like a plain list assembly).
    An Assembly is an iterable of specs, so it goes straight into
    ``canon.DecomposedCalculator(assembly)``.
    """

    def __init__(self, constraints=None):
        self._specs: list[Constraint] = []
        if constraints is None:
            self._specs = [cls() for cls in _SPEC_CLASSES]
        else:
            for constraint in constraints:
                self.add(constraint)

    def add(
        self,
        constraint: str | Constraint,
        *,
        weight: float | None = None,
        hard: bool | None = None,
        exponent: float = 1.0,
    ) -> "Assembly":
        """Adds a constraint (replacing one with the same name). A name builds a
        spec from weight/hard/exponent; a spec instance is used as is (mixing an
        instance with keyword overrides raises ValueError). Unknown names raise
        ValueError.

        cpsat names (`CPSAT_ALIASES`) are accepted too, so the same assembly
        declaration can go to both backends unchanged."""
        if isinstance(constraint, str):
            spec_class = _SPEC_BY_NAME.get(resolve_name(constraint))
            if spec_class is None:
                raise ValueError(f"unknown constraint name: {constraint!r} (see list_constraints())")
            spec = spec_class(weight=weight, hard=hard, exponent=exponent)
        elif isinstance(constraint, Constraint):
            if weight is not None or hard is not None or exponent != 1.0:
                raise ValueError("a spec instance cannot be combined with weight/hard/exponent (put them in the instance)")
            spec = constraint
        else:
            raise TypeError(f"expected a constraint name (str) or a Constraint spec: {constraint!r}")
        for i, existing in enumerate(self._specs):
            if existing.name == spec.name:
                self._specs[i] = spec
                return self
        self._specs.append(spec)
        return self

    def drop(self, name: str) -> "Assembly":
        """Removes the named constraint from the assembly. ValueError if absent (guards against typos).

        Like `add`, cpsat names are accepted too."""
        resolved = resolve_name(name)
        remaining = [spec for spec in self._specs if spec.name != resolved]
        if len(remaining) == len(self._specs):
            raise ValueError(f"cannot drop a constraint that is not in the assembly: {name!r}")
        self._specs = remaining
        return self

    def __iter__(self):
        return iter(self._specs)

    def __len__(self) -> int:
        return len(self._specs)

    def __contains__(self, name: str) -> bool:
        return any(spec.name == CPSAT_ALIASES.get(name, name) for spec in self._specs)

    def __repr__(self) -> str:
        return f"Assembly([{', '.join(spec.name for spec in self._specs)}])"


def get_constraints(
    constraints: list[Constraint] | None = None,
) -> list[Constraint]:
    """Returns the cost function as a list of specs. Without an argument, all fifteen default constraints.

    The same entry point as every other use case in the repository, so "the
    defaults with a small change" reads the same everywhere::

        from st_timetabling.constraints import TeacherGap, get_constraints

        canon.DecomposedCalculator(get_constraints())          # defaults as is
        canon.DecomposedCalculator(get_constraints([           # pick and choose
            ClassConflict(), TeacherGap(weight=40),
        ]))

    Given `constraints`, that list is used as is — only the listed constraints
    are scored. The default set equals what :class:`Assembly` builds without
    arguments; use it when you want name-based add/drop chaining.
    """
    if constraints is None:
        return [cls() for cls in _SPEC_CLASSES]
    if not constraints:
        # Same contract as C++ applyConstraints: an empty list means "score
        # nothing", which is most likely a mistake. A filter that happens to
        # come out empty must not silently flip to "score everything".
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return list(constraints)
