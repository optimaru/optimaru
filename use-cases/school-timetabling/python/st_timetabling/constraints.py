"""Canonical constraint specs — an API to build the cost function from plugins.

The default fast path (the monolithic incremental calculator) scores every
canonical constraint at its default weight. This module is the optional path
next to it: pick constraints one by one, change their weights, flip their
hard/soft level, drop some, then pass the selection to
``solve(problem, constraints=...)`` or ``canon.DecomposedCalculator``.

The shortest form is :func:`get_constraints` — without an argument it is the
default set, and a list replaces it::

    import st_timetabling as st
    from st_timetabling.constraints import TeacherGapCost, class_conflict_cost, get_constraints

    st.solve(problem, constraints=get_constraints())               # defaults as they are
    st.solve(problem, constraints=get_constraints([                # only these two
        class_conflict_cost, TeacherGapCost(weight=40),
    ]))

:class:`Assembly` starts from the default set and edits it by name, so "the
defaults with a small change" is a one-liner::

    from st_timetabling.constraints import Assembly

    st.solve(problem, constraints=Assembly().add("teacher_gap_cost", weight=40).drop("teacher_workday_cost"))

Every spec takes the same three knobs, ``(weight=None, level=None, exponent=1.0)``:

- ``weight``   linear multiplier of the constraint's raw cost. ``None`` is the
               component's default weight (the C++ kDefaultWeight: 1 for the
               hard constraints, 20/3/30/10/5/1 for the six soft ones), never
               a weight stored anywhere else. ``st.constraint_info()`` reports
               the defaults.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level.
- ``exponent`` sign-preserving power shaping of the cost: ``weight * sign(c) *
               |c| ** exponent`` (per slot for the per-slot constraints, on the
               single total for the others). 2.0 = squared.

Names are the C++ component names in snake_case (``TeacherGapCost`` ->
``teacher_gap_cost``): the one name of each constraint in every layer —
``list_constraints()``, ``constraint_info()``, ``constraint_costs()``, the
CP-SAT reference, the ``Timetable.penalties`` keys and the solve ``weights``
keys. Module-level instances at the default weights (``teacher_gap_cost``, ...)
are ready to pick.
"""

from ._constraint import Constraint, Level, Score, score_costs, select
from ._core import canon, constraint_info

__all__ = [
    "UNASSIGNED_LESSONS_SOFT_WEIGHT",
    "Assembly",
    "AvoidedPeriodCost",
    "BlockAdjacencyCost",
    "ClassConflictCost",
    "ClassGapCost",
    "Constraint",
    "CourseMaxPeriodsPerDayCost",
    "DisallowedSlotCost",
    "FixedSlotsCost",
    "NonPreferredPeriodCost",
    "ParallelGroupsCost",
    "RoomCapacityCost",
    "Score",
    "TeacherConflictCost",
    "TeacherFreeDayCost",
    "TeacherGapCost",
    "TeacherMaxConsecutivePeriodsCost",
    "TeacherMaxPeriodsCost",
    "TeacherWorkdayCost",
    "UnassignedLessonsCost",
    "avoided_period_cost",
    "block_adjacency_cost",
    "class_conflict_cost",
    "class_gap_cost",
    "course_max_periods_per_day_cost",
    "default_weights",
    "disallowed_slot_cost",
    "fixed_slots_cost",
    "get_constraints",
    "list_constraints",
    "non_preferred_period_cost",
    "parallel_groups_cost",
    "room_capacity_cost",
    "score",
    "soft_specs",
    "spec_named",
    "teacher_conflict_cost",
    "teacher_free_day_cost",
    "teacher_gap_cost",
    "teacher_max_consecutive_periods_cost",
    "teacher_max_periods_cost",
    "teacher_workday_cost",
    "unassigned_lessons_cost",
]

#: The documented weight of ``unassigned_lessons_cost`` demoted to soft
#: (partial timetabling, the server's ``allow_partial``): the price of one
#: unplaced weekly period, well above every other soft term, so that every
#: period that fits is placed. C++ ``UnassignedLessonsCost::kSoftWeight``. It is
#: a recommendation, not a default — ``weight=None`` keeps the component
#: default (1) on either level::
#:
#:     UnassignedLessonsCost(level="soft", weight=UNASSIGNED_LESSONS_SOFT_WEIGHT)
UNASSIGNED_LESSONS_SOFT_WEIGHT: float = float(canon.UNASSIGNED_LESSONS_SOFT_WEIGHT)


def list_constraints() -> list[str]:
    """Every constraint name a selection accepts, in catalog order (``canon.CONSTRAINT_NAMES``)."""
    return list(canon.CONSTRAINT_NAMES)


class UnassignedLessonsCost(Constraint):
    """Weekly periods left without a slot. Default: hard, weight 1.

    Demoting it with ``level="soft"`` gives **partial timetabling** (the CP-SAT
    reference's relaxed weekly-period counts): the search gains unassign moves,
    so "leave it out" becomes an option, and the unfilled periods are reported
    as ``penalties["unassigned_lessons_cost"]`` instead of a hard violation.
    The weight is then the price of one unassigned period: give it
    :data:`UNASSIGNED_LESSONS_SOFT_WEIGHT` (10000) — at the default 1 a lesson
    would be dropped to save a single teacher gap."""

    name = "unassigned_lessons_cost"
    label_en = "Unassigned lessons"
    label_ko = "미배정 수업"
    description_en = (
        "Weekly periods of a course that have no time slot. One unit of cost is one unplaced period; "
        "0 means every course gets all of its weekly periods."
    )
    description_ko = (
        "강좌의 주당 시수 가운데 시간이 배정되지 않은 수업 수입니다. 배정되지 않은 수업 1시간이 비용 1이며, "
        "0이면 모든 강좌가 주당 시수를 다 채운 것입니다."
    )


class ClassConflictCost(Constraint):
    """Pairs of one class's lessons at the same time. Default: hard, weight 1."""

    name = "class_conflict_cost"
    label_en = "Class double-booking"
    label_ko = "학급 중복 배정"
    description_en = (
        "Pairs of lessons of the same class placed in the same period (a parallel group counts once). "
        "One unit of cost is one clashing pair; 0 means no class has two lessons at once."
    )
    description_ko = (
        "같은 학급의 수업이 같은 교시에 겹친 쌍의 수입니다(분반 묶음은 한 수업으로 셉니다). "
        "겹친 한 쌍이 비용 1이며, 0이면 어느 학급도 한 교시에 수업이 둘 이상 없습니다."
    )


class TeacherConflictCost(Constraint):
    """Pairs of one teacher's lessons at the same time, team teaching included. Default: hard, weight 1."""

    name = "teacher_conflict_cost"
    label_en = "Teacher double-booking"
    label_ko = "교사 중복 배정"
    description_en = (
        "Pairs of lessons in the same period that share a teacher, counted once per shared teacher "
        "(team teaching included). One unit of cost is one clashing pair; 0 means no teacher stands in two "
        "lessons at once."
    )
    description_ko = (
        "같은 교시에 같은 교사가 맡은 수업 쌍의 수로, 공동 수업이면 겹친 교사마다 셉니다. "
        "겹친 한 쌍이 비용 1이며, 0이면 어느 교사도 한 교시에 두 수업을 맡지 않습니다."
    )


class DisallowedSlotCost(Constraint):
    """Lessons on a time their course may not use. Default: hard, weight 1.

    Forbidden slots and teacher/class unavailability are folded into one
    allowed-slot table by the conversion. Engine-only: the CP-SAT reference
    never creates a variable for such a time."""

    name = "disallowed_slot_cost"
    label_en = "Disallowed times"
    label_ko = "배정 불가 시간"
    description_en = (
        "Lessons placed on a time the course may not use: a forbidden slot, or a time a teacher or the "
        "class is unavailable. One unit of cost is one such lesson; 0 means every lesson sits on an allowed time."
    )
    description_ko = (
        "강좌가 쓸 수 없는 시간(금지 시간, 교사나 학급의 불가 시간)에 놓인 수업 수입니다. "
        "그런 수업 1개가 비용 1이며, 0이면 모든 수업이 허용된 시간에 있습니다."
    )


class RoomCapacityCost(Constraint):
    """Special-room demand beyond the rooms of that type, per time. Default: hard, weight 1."""

    name = "room_capacity_cost"
    label_en = "Special-room capacity"
    label_ko = "특별실 수용량"
    description_en = (
        "Per special-room type and period, the lessons that need such a room beyond the rooms the school has. "
        "One unit of cost is one lesson without a room; 0 means every special-room lesson finds a room."
    )
    description_ko = (
        "특별실 종류와 교시마다 그 특별실이 필요한 수업 가운데 보유한 실 수를 넘는 수입니다. "
        "특별실을 배정받지 못한 수업 1개가 비용 1이며, 0이면 특별실이 필요한 수업이 모두 특별실을 배정받습니다."
    )


class CourseMaxPeriodsPerDayCost(Constraint):
    """Lessons of a course beyond its daily cap (max_periods_per_day). Default: hard, weight 1."""

    name = "course_max_periods_per_day_cost"
    label_en = "Course periods per day"
    label_ko = "강좌 일일 시수 상한"
    description_en = (
        "Per course and day, the lessons beyond the course's max_periods_per_day. One unit of cost is one "
        "lesson over the cap; 0 means no course exceeds its daily cap."
    )
    description_ko = (
        "강좌와 요일마다 max_periods_per_day를 넘긴 수업 수입니다. 상한을 넘은 수업 1개가 비용 1이며, "
        "0이면 어느 강좌도 하루 상한을 넘지 않습니다."
    )


class BlockAdjacencyCost(Constraint):
    """How far a block (block_periods consecutive periods) is broken. Default: hard, weight 1."""

    name = "block_adjacency_cost"
    label_en = "Consecutive blocks"
    label_ko = "연강 배치"
    description_en = (
        "For each block of consecutive periods, the missing adjacencies on one day plus one per member left "
        "out while the rest is placed. 0 means every block sits on one day in consecutive periods."
    )
    description_ko = (
        "연강 묶음마다 같은 날 바로 이어지지 못한 곳의 수에, 일부만 배정됐을 때 빠진 수업 수를 더합니다. "
        "0이면 모든 연강이 하루 안에 연달아 놓인 것입니다."
    )


class TeacherMaxPeriodsCost(Constraint):
    """A teacher's periods beyond the daily and weekly caps. Default: hard, weight 1."""

    name = "teacher_max_periods_cost"
    label_en = "Teacher period limits"
    label_ko = "교사 시수 상한"
    description_en = (
        "Per teacher, the periods beyond max_periods_per_day on each day plus the periods beyond "
        "max_periods_per_week in the week. One unit of cost is one period over a cap; 0 means every teacher "
        "is within both caps."
    )
    description_ko = (
        "교사마다 하루 max_periods_per_day를 넘긴 시수와 주 max_periods_per_week를 넘긴 시수를 더합니다. "
        "상한을 넘은 1시간이 비용 1이며, 0이면 모든 교사가 두 상한을 지킵니다."
    )


class TeacherMaxConsecutivePeriodsCost(Constraint):
    """Periods beyond a teacher's max_consecutive_periods in a row. Default: hard, weight 1."""

    name = "teacher_max_consecutive_periods_cost"
    label_en = "Teacher consecutive periods"
    label_ko = "교사 연속 수업 제한"
    description_en = (
        "Per teacher and day, the periods of each unbroken run beyond max_consecutive_periods. One unit of "
        "cost is one period over the limit; 0 means no teacher teaches longer runs than allowed."
    )
    description_ko = (
        "교사와 요일마다 쉬지 않고 이어진 수업이 max_consecutive_periods를 넘긴 시수입니다. "
        "넘긴 1시간이 비용 1이며, 0이면 어느 교사도 허용보다 길게 연달아 가르치지 않습니다."
    )


class TeacherGapCost(Constraint):
    """Idle periods inside a teacher's day. Default: soft, weight 20."""

    name = "teacher_gap_cost"
    label_en = "Teacher gaps"
    label_ko = "교사 공강"
    description_en = (
        "Free periods between a teacher's first and last lesson of a day, summed over teachers and days. "
        "One unit of cost is one idle period; 0 means every teacher's day is one compact run."
    )
    description_ko = (
        "교사의 하루 첫 수업과 마지막 수업 사이에 빈 교시 수를 교사와 요일에 걸쳐 더합니다. "
        "빈 교시 1개가 비용 1이며, 0이면 모든 교사의 하루가 빈틈없이 이어집니다."
    )


class TeacherWorkdayCost(Constraint):
    """(teacher, day) pairs with any lesson. Default: soft, weight 3."""

    name = "teacher_workday_cost"
    label_en = "Teacher workdays"
    label_ko = "교사 출근일"
    description_en = (
        "The days each teacher has at least one lesson, summed over teachers. One unit of cost is one "
        "teacher-day; it pulls each teacher's lessons onto fewer days (0 only without lessons)."
    )
    description_ko = (
        "교사마다 수업이 하나라도 있는 요일 수를 모두 더합니다. 교사의 출근 하루가 비용 1이며, "
        "수업을 더 적은 요일로 모으게 합니다(수업이 없을 때만 0입니다)."
    )


class TeacherFreeDayCost(Constraint):
    """Worked requested free days: teacher-days with lessons on a preferred free day. Default: soft, weight 30."""

    name = "teacher_free_day_cost"
    label_en = "Worked requested free days"
    label_ko = "희망 공강일 출근"
    description_en = (
        "Days a teacher asked to keep free (preferred_free_days) on which they still have a lesson. "
        "One unit of cost is one worked requested free day; 0 means every requested free day stays free."
    )
    description_ko = (
        "교사가 비워 두기를 희망한 요일(preferred_free_days)에 수업이 잡힌 날 수입니다. "
        "희망 공강일에 출근하는 하루가 비용 1이며, 0이면 희망 공강일이 모두 비어 있습니다."
    )


class ClassGapCost(Constraint):
    """Idle periods inside a class's day. Default: soft, weight 10."""

    name = "class_gap_cost"
    label_en = "Class gaps"
    label_ko = "학급 공강"
    description_en = (
        "Free periods between a class's first and last lesson of a day, summed over classes and days. "
        "One unit of cost is one idle period; 0 means every class day is one compact run."
    )
    description_ko = (
        "학급의 하루 첫 수업과 마지막 수업 사이에 빈 교시 수를 학급과 요일에 걸쳐 더합니다. "
        "빈 교시 1개가 비용 1이며, 0이면 모든 학급의 하루가 빈틈없이 이어집니다."
    )


class AvoidedPeriodCost(Constraint):
    """Lessons in a period their subject avoids. Default: soft, weight 5.

    A parallel group counts once per member subject."""

    name = "avoided_period_cost"
    label_en = "Avoided periods"
    label_ko = "기피 교시"
    description_en = (
        "Lessons placed in a period their subject lists in avoided_periods (a parallel group counts once per "
        "member course). One unit of cost is one such lesson; 0 means no lesson sits in an avoided period."
    )
    description_ko = (
        "과목이 avoided_periods로 피하려는 교시에 놓인 수업 수입니다(분반 묶음은 강좌마다 셉니다). "
        "그런 수업 1개가 비용 1이며, 0이면 기피 교시에 놓인 수업이 없습니다."
    )


class NonPreferredPeriodCost(Constraint):
    """Lessons outside their subject's preferred periods, when it has some. Default: soft, weight 1."""

    name = "non_preferred_period_cost"
    label_en = "Non-preferred periods"
    label_ko = "선호 교시 이탈"
    description_en = (
        "Lessons whose subject lists preferred_periods but that sit in another period (a subject without "
        "preferences never counts). One unit of cost is one such lesson; 0 means every lesson with a "
        "preference sits in a preferred period."
    )
    description_ko = (
        "과목에 preferred_periods가 있는데 그 밖의 교시에 놓인 수업 수입니다(선호가 없는 과목은 세지 않습니다). "
        "그런 수업 1개가 비용 1이며, 0이면 선호 교시가 있는 수업이 모두 선호 교시에 놓입니다."
    )


class FixedSlotsCost(Constraint):
    """Lessons off their course's fixed slots. Default: hard, weight 1; structural, not flippable.

    Always 0 in the engine: every fixed slot becomes a pinned lesson the
    search never moves. ``level="soft"`` is refused."""

    name = "fixed_slots_cost"
    label_en = "Fixed slots"
    label_ko = "고정 시간"
    description_en = (
        "Lessons of a course that are not at the course's fixed_slots. The engine pins every fixed lesson, "
        "so the cost is 0 by construction; it cannot be made soft."
    )
    description_ko = (
        "강좌의 fixed_slots에 놓이지 않은 수업 수입니다. 엔진은 고정 수업을 그 시간에 묶어 두므로 "
        "비용은 늘 0이며, 소프트 제약으로 바꿀 수 없습니다."
    )


class ParallelGroupsCost(Constraint):
    """Sections of one parallel group at different times. Default: hard, weight 1; structural, not flippable.

    Always 0 in the engine: a parallel group is merged into one lesson per
    weekly period. ``level="soft"`` is refused."""

    name = "parallel_groups_cost"
    label_en = "Parallel groups"
    label_ko = "분반 동시 배치"
    description_en = (
        "Sections of one parallel_group (a class split into groups taught at the same time) placed at "
        "different times. The engine merges each group into one lesson, so the cost is 0 by construction; "
        "it cannot be made soft."
    )
    description_ko = (
        "같은 parallel_group(한 학급을 나눠 동시에 수업하는 분반)의 수업이 서로 다른 시간에 놓인 수입니다. "
        "엔진은 분반을 한 수업으로 합치므로 비용은 늘 0이며, 소프트 제약으로 바꿀 수 없습니다."
    )


#: Instances at the default weights (weight=None). Reference them directly when picking.
unassigned_lessons_cost = UnassignedLessonsCost()
class_conflict_cost = ClassConflictCost()
teacher_conflict_cost = TeacherConflictCost()
disallowed_slot_cost = DisallowedSlotCost()
room_capacity_cost = RoomCapacityCost()
course_max_periods_per_day_cost = CourseMaxPeriodsPerDayCost()
block_adjacency_cost = BlockAdjacencyCost()
teacher_max_periods_cost = TeacherMaxPeriodsCost()
teacher_max_consecutive_periods_cost = TeacherMaxConsecutivePeriodsCost()
teacher_gap_cost = TeacherGapCost()
teacher_workday_cost = TeacherWorkdayCost()
teacher_free_day_cost = TeacherFreeDayCost()
class_gap_cost = ClassGapCost()
avoided_period_cost = AvoidedPeriodCost()
non_preferred_period_cost = NonPreferredPeriodCost()
fixed_slots_cost = FixedSlotsCost()
parallel_groups_cost = ParallelGroupsCost()

# The default set, in list_constraints() order.
_DEFAULTS = (
    unassigned_lessons_cost,
    class_conflict_cost,
    teacher_conflict_cost,
    disallowed_slot_cost,
    room_capacity_cost,
    course_max_periods_per_day_cost,
    block_adjacency_cost,
    teacher_max_periods_cost,
    teacher_max_consecutive_periods_cost,
    teacher_gap_cost,
    teacher_workday_cost,
    teacher_free_day_cost,
    class_gap_cost,
    avoided_period_cost,
    non_preferred_period_cost,
    fixed_slots_cost,
    parallel_groups_cost,
)

_SPEC_BY_NAME: dict[str, type[Constraint]] = {type(spec).name: type(spec) for spec in _DEFAULTS}


def soft_specs() -> tuple[type[Constraint], ...]:
    """The spec classes that are soft by default, in catalog order — the terms of the objective.

    Read off ``constraint_info()`` (the C++ kDefaultLevel), so it follows a new
    soft component by itself."""
    return tuple(_SPEC_BY_NAME[row.name] for row in constraint_info() if row.default_level == "soft")


def default_weights() -> dict[str, float]:
    """Soft constraint name -> its default weight (the C++ kDefaultWeight, via ``constraint_info()``)."""
    return {row.name: row.default_weight for row in constraint_info() if row.default_level == "soft"}


def spec_named(name: str, *, weight=None, level=None, exponent=1.0) -> Constraint:
    """The spec of the constraint called ``name`` with the given knobs; unknown names raise ValueError."""
    spec_class = _SPEC_BY_NAME.get(name)
    if spec_class is None:
        raise ValueError(f"unknown constraint name: {name!r} (see list_constraints())")
    return spec_class(weight=weight, level=level, exponent=exponent)


class Assembly:
    """Canonical constraint assembly builder. It starts from the whole default
    set — the same configuration as the monolithic default path — so "the
    defaults with a small change" stays short.

    ``add`` inserts a constraint by name (or spec instance) and **replaces the
    one with the same name** (override semantics; for summed duplicates use
    ``canon.DecomposedCalculator.add`` directly). ``drop`` removes the named
    constraint. Both return self for chaining::

        Assembly().add("teacher_gap_cost", weight=40).drop("teacher_workday_cost")

    ``Assembly(constraints)`` with an iterable of spec instances/names starts
    from that list instead of the default set. An Assembly is an iterable of
    specs, so it goes straight into ``canon.DecomposedCalculator(assembly)``.
    """

    def __init__(self, constraints=None):
        self._specs: list[Constraint] = []
        if constraints is None:
            self._specs = list(_DEFAULTS)
        else:
            for constraint in constraints:
                self.add(constraint)

    def add(
        self,
        constraint: str | Constraint,
        *,
        weight: float | None = None,
        level: Level | str | None = None,
        exponent: float = 1.0,
    ) -> "Assembly":
        """Adds a constraint (replacing one with the same name). A name builds a
        spec from weight/level/exponent; a spec instance is used as is (mixing an
        instance with keyword overrides raises ValueError). Unknown names raise
        ValueError."""
        if isinstance(constraint, str):
            spec = spec_named(constraint, weight=weight, level=level, exponent=exponent)
        elif isinstance(constraint, Constraint):
            if weight is not None or level is not None or exponent != 1.0:
                raise ValueError(
                    "a spec instance cannot be combined with weight/level/exponent (put them in the instance)"
                )
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
        """Removes the named constraint from the assembly. ValueError if absent (guards against typos)."""
        remaining = [spec for spec in self._specs if spec.name != name]
        if len(remaining) == len(self._specs):
            raise ValueError(f"cannot drop a constraint that is not in the assembly: {name!r}")
        self._specs = remaining
        return self

    def __iter__(self):
        return iter(self._specs)

    def __len__(self) -> int:
        return len(self._specs)

    def __contains__(self, name: str) -> bool:
        return any(spec.name == name for spec in self._specs)

    def __repr__(self) -> str:
        return f"Assembly([{', '.join(spec.name for spec in self._specs)}])"


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """The cost function as a list of specs: ``None`` is the default set (all
    seventeen, in list_constraints() order); a list replaces it — only the
    listed constraints are scored::

        canon.DecomposedCalculator(get_constraints())          # the defaults
        canon.DecomposedCalculator(get_constraints([           # pick and choose
            class_conflict_cost, TeacherGapCost(weight=40),
        ]))

    The same contract as the C++ ``applyConstraints``: an empty list is an error
    (not "the defaults"), and so is naming a constraint twice. The default set
    equals what :class:`Assembly` builds without arguments.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Scores raw constraint costs with a selection — pure Python, the engine's arithmetic.

    ``costs`` is ``canon.constraint_costs(table)`` (each constraint's raw cost
    per slot, or its single total, before weight / exponent / level);
    ``constraints`` the selection (``None`` = the defaults). Equals the engine's
    score of the same assembly on the same table, without a solver."""
    return score_costs(costs, get_constraints(constraints))
