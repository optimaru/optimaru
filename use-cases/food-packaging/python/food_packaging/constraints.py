"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`fp.solve(schedule)`) enables all five constraints at the default weights.
This module is the alternative path next to it: pick constraints, change weights, change
levels, shape the cost, and pass the list to `solve(schedule, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import food_packaging as fp
    from food_packaging.constraints import (
        MaxEndTimeCost, SquaredMakespanCost, UnassignedJobsCost, get_constraints,
    )

    fp.solve(schedule, constraints=get_constraints())           # the defaults as-is
    fp.solve(schedule, constraints=[                             # picked
        MaxEndTimeCost(), UnassignedJobsCost(), SquaredMakespanCost(weight=10),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``max_end_time_cost`` is
``MaxEndTimeCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost, in units of its level.
               ``None`` = the component default (``fp.constraint_info()`` reports
               them).
- ``level``    ``None`` = the default level, otherwise a ``Level`` or its name
               (``Level.MEDIUM`` or ``"medium"``). Any flippable constraint may
               take any of the three levels. ``AfterIdealEndTimeCost`` and
               ``UnassignedJobsCost`` are MEDIUM constraints by default:
               ``level="hard"`` makes them one hard unit per minute and
               ``level="soft"`` one soft unit per
               minute (outside the 10^10 medium fold), in the solver and in
               :func:`score` alike. A ``weight`` is in units of the level in effect.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``, per line for the unit constraints and on the single total
               for the globals ``OperatorCleaningConflictCost`` and
               ``UnassignedJobsCost``. 2.0 = squared, so concentrated violations
               grow much costlier.
- ``offset``   (keyword) shifts that shaping, at the same granularity; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a line below the offset lowers the
               penalty. The literal form charges every unit the constraint
               evaluates, a raw 0 included: every line (an empty one included,
               never the unassigned pool, which is no line) for the per-line
               constraints, the one total for a global; a ``constraint_costs()``
               entry lists the lines as ``"units"``. With an offset it is refused
               on hard (``MaxEndTimeCost`` and ``OperatorCleaningConflictCost``
               by default), because hard 0 must mean no violation; medium folds
               into soft here, so it takes the literal formula.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs and the picks add up. Lateness past the deadline per line, plus its
square beyond 30 minutes::

    fp.solve(schedule, constraints=[
        MaxEndTimeCost(),
        MaxEndTimeCost(exponent=2, offset=30),
        *[spec for spec in get_constraints() if spec.name != "max_end_time_cost"],
    ])

``fp.constraint_costs(schedule, picks)`` keys the second pick ``"max_end_time_cost#2"``.

Names (`name`) are the C++ component names in snake_case
(``MaxEndTimeCost`` -> ``max_end_time_cost``). The core derives them mechanically,
so there is no hand-maintained alias table — check them with `fp.list_constraints()`.
Module-level instance names use the same snake_case, so they match the strings
`list_constraints()` prints.
"""

from ._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "MaxEndTimeCost",
    "AfterIdealEndTimeCost",
    "SquaredMakespanCost",
    "OperatorCleaningConflictCost",
    "UnassignedJobsCost",
    "max_end_time_cost",
    "after_ideal_end_time_cost",
    "squared_makespan_cost",
    "operator_cleaning_conflict_cost",
    "unassigned_jobs_cost",
    "get_constraints",
]


class MaxEndTimeCost(Constraint):
    """Minutes by which a job passes the end time it must not miss. Default: hard, weight 1.

    Unassigned jobs are silent here — `unassigned_jobs_cost` prices them."""

    name = "max_end_time_cost"
    label_en = "Job past its latest end time"
    label_ko = "최종 마감 시각 초과"
    description_en = (
        "Minutes by which an assigned job ends after its latest allowed end time (max_end_time), "
        "summed over the jobs of every line. One unit of cost is one minute late; 0 means every "
        "assigned job ends by its deadline. Jobs on no line are not counted here."
    )
    description_ko = (
        "라인에 배정된 작업이 최종 마감 시각(max_end_time)보다 늦게 끝난 시간(분)을 모든 라인에 걸쳐 더합니다. "
        "1분 늦을 때마다 비용 1이며, 0이면 배정된 모든 작업이 최종 마감 시각까지 끝납니다. "
        "라인에 배정되지 않은 작업은 여기서 세지 않습니다."
    )


class AfterIdealEndTimeCost(Constraint):
    """Minutes by which a job passes its ideal end time. Default: medium, weight 1 (medium units).

    Missing the ideal time is a cost; missing `max_end_time_cost` is a violation —
    that is why the shape is the same but the level differs. The domain folds medium
    into the engine soft level as ``medium * 10^10``; the fold belongs to the domain
    (every ``constraint_costs()`` entry carries it as ``medium_fold``), so the weight
    carries no 10^10 factor."""

    name = "after_ideal_end_time_cost"
    label_en = "Job past its ideal end time"
    label_ko = "이상적 종료 시각 초과"
    description_en = (
        "Minutes by which an assigned job ends after its ideal end time (ideal_end_time), summed "
        "over the jobs of every line. One unit of cost is one minute past the ideal end; 0 means "
        "every assigned job ends by its ideal time."
    )
    description_ko = (
        "라인에 배정된 작업이 이상적인 종료 시각(ideal_end_time)보다 늦게 끝난 시간(분)을 모든 라인에 걸쳐 "
        "더합니다. 1분 늦을 때마다 비용 1이며, 0이면 배정된 모든 작업이 이상적인 종료 시각까지 끝납니다."
    )


class SquaredMakespanCost(Constraint):
    """The **square** of each line's busy time. Default: soft, weight 1.

    Squaring pushes toward spreading jobs evenly across lines rather than just shrinking
    the total. An empty line costs nothing.

    The square lives inside the raw cost rather than in `exponent` on purpose — the
    exponent shapes the raw cost on top of it, so callers can still layer more shaping."""

    name = "squared_makespan_cost"
    label_en = "Squared line makespan"
    label_ko = "라인 가동 시간의 제곱"
    description_en = (
        "For every line with jobs, the square of the minutes from the line start to the end of "
        "its last job, summed over the lines; squaring favours spreading the work evenly. One "
        "unit of cost is one squared minute; 0 means no line has a job."
    )
    description_ko = (
        "작업이 있는 라인마다 라인 시작부터 마지막 작업이 끝날 때까지의 시간(분)을 제곱해 모두 더합니다. "
        "제곱하기 때문에 작업을 라인에 고르게 나눌수록 유리합니다. 분 단위 시간을 제곱한 값 1(1분²)이 "
        "비용 1이며, 0이면 어느 라인에도 작업이 없습니다."
    )


class OperatorCleaningConflictCost(Constraint):
    """Overlapping cleaning minutes on lines sharing one operator. Default: hard, weight 1.

    One person cannot clean two machines at once. The conflicting jobs sit on
    **different** lines, so this is a global constraint (two jobs on one line have
    strictly sequential cleaning windows and cannot overlap)."""

    name = "operator_cleaning_conflict_cost"
    label_en = "Operator cleaning overlap"
    label_ko = "작업자 세척 시간 겹침"
    description_en = (
        "Minutes in which the cleaning windows of two jobs overlap on lines run by the same "
        "operator, summed over every such pair of jobs. One unit of cost is one overlapping "
        "minute; 0 means no operator has two cleanings at the same time."
    )
    description_ko = (
        "같은 작업자가 맡은 라인들에서 두 작업의 세척 시간이 겹치는 시간(분)을 그러한 모든 작업 쌍에 대해 "
        "더합니다. 겹치는 1분이 비용 1이며, 0이면 어떤 작업자도 두 세척을 동시에 하지 않습니다."
    )


class UnassignedJobsCost(Constraint):
    """Production minutes of jobs on no packaging line. Default: medium, weight 1 (medium units).

    Dropping a long job hurts more than dropping a short one. A job can be in no list
    at all (the initial state before the construction heuristic), so the condition is
    "on no line", which is global rather than a property of any single unit. The
    domain's medium fold (``medium * 10^10`` into soft) lives in the domain, not in the
    weight."""

    name = "unassigned_jobs_cost"
    label_en = "Unassigned jobs"
    label_ko = "미배정 작업"
    description_en = (
        "The production minutes of every job left on no packaging line, so leaving out a long "
        "job costs more than a short one. One unit of cost is one unscheduled production "
        "minute; 0 means every job is on a line."
    )
    description_ko = (
        "어느 포장 라인에도 배정되지 않은 작업의 생산 시간(분)을 모두 더합니다. 따라서 긴 작업을 빼면 짧은 "
        "작업을 뺄 때보다 비용이 큽니다. 배정되지 않은 작업의 생산 시간 1분이 비용 1이며, 0이면 모든 작업이 "
        "라인에 배정되어 있습니다."
    )


#: Ready-made default instances (``max_end_time_cost`` is ``MaxEndTimeCost()``).
max_end_time_cost = MaxEndTimeCost()
after_ideal_end_time_cost = AfterIdealEndTimeCost()
squared_makespan_cost = SquaredMakespanCost()
operator_cleaning_conflict_cost = OperatorCleaningConflictCost()
unassigned_jobs_cost = UnassignedJobsCost()

#: Default set — same order as `fp.list_constraints()` (unit first, global after).
_DEFAULTS = (
    max_end_time_cost,
    after_ideal_end_time_cost,
    squared_makespan_cost,
    operator_cleaning_conflict_cost,
    unassigned_jobs_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all five default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([MaxEndTimeCost(), SquaredMakespanCost(weight=10)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) raises ValueError; a constraint named several times is
    scored once per pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `fp.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent, offset and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = fp.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, [MaxEndTimeCost(), SquaredMakespanCost(weight=10)])
        score(raw, [MaxEndTimeCost(), MaxEndTimeCost(exponent=2, offset=30)])

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent, offset, clip)`` and
    hard contributions are **rounded per unit** before summing (the same order as
    the engine; rounding the total afterwards gives different values). A second
    pick of a constraint reads the same raw costs with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft): the domain's medium
    fold (``medium_fold`` on every entry, into soft at 10^10) is applied, so it
    equals ``(score.hard, score.medium * 10**10 + score.soft)`` of a schedule scored
    by the same constraints.
    """
    return score_costs(costs, get_constraints(constraints))
