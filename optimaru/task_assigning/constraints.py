"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`ta.solve(solution)`) enables all six constraints at their default
weights. This module is the alternative path next to it: pick constraints, change weights,
change levels, shape the cost, and pass the list to `solve(solution, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import optimaru.task_assigning as ta
    from optimaru.task_assigning.constraints import (
        RequiredSkillCost, SquaredMakespanCost, UnassignedTasksCost, get_constraints,
    )

    ta.solve(solution, constraints=get_constraints())           # the defaults as-is
    ta.solve(solution, constraints=[                             # picked
        RequiredSkillCost(), UnassignedTasksCost(), SquaredMakespanCost(weight=10),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``required_skill_cost``
is ``RequiredSkillCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost, in units of its level.
               ``None`` = the C++ component default (``ta.constraint_info()``
               lists them).
- ``level``    ``None`` = the default level, otherwise a ``Level`` or its name
               (``Level.MEDIUM`` or ``"medium"``). Any flippable constraint may
               take any of the three levels.
               ``UnassignedTasksCost`` is the one **medium** constraint by default
               (soft[0], folded into the engine's hard level BELOW the
               missing skills: ``engine.hard = hard * 10^9 + medium``): ``level="hard"`` charges one hard unit
               (10^9 engine-hard units) per unassigned task, ``level="soft"`` one
               soft unit per unassigned task, and a ``weight`` is in units of the level in effect.
               A constraint moved onto medium counts in that below-hard level too,
               rounded per unit like a hard one.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``, per employee for the unit constraints and on the total
               for the global ``UnassignedTasksCost``. 2.0 = squared, so
               concentrated violations grow much costlier.
- ``offset``   (keyword) shifts that shaping, at the same granularity; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so an employee below the offset — an idle
               one (cost 0) included — lowers the penalty. With an offset it is
               refused on hard (``RequiredSkillCost`` by default) and on medium
               (``UnassignedTasksCost`` by default), since medium folds below
               hard here and so counts toward the hard score, and hard 0 must
               mean no violation.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs and the picks add up. The critical end times per employee, plus the
square of the part beyond 120 minutes::

    ta.solve(solution, constraints=[
        *get_constraints(),
        CriticalPriorityTaskEndTimeCost(exponent=2, offset=120),
    ])

``ta.constraint_costs(solution, picks)`` keys the second pick
``"critical_priority_task_end_time_cost#2"``.

This domain scores four score levels (hard[0], soft[0..2]) on the engine's
three: hard (``required_skill_cost``), medium (``unassigned_tasks_cost``) and soft
(the other four). soft[1] and soft[2] share the soft level; the default weight 10^7 of
``squared_makespan_cost`` is what keeps the makespan term above the priority end
times, so a weight you pass is a share within the same level (see `cpp/include/ta/domain.hpp`).

Names (`name`) are the C++ component names in snake_case
(``RequiredSkillCost`` -> ``required_skill_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`ta.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

from .._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "CriticalPriorityTaskEndTimeCost",
    "Level",
    "MajorPriorityTaskEndTimeCost",
    "MinorPriorityTaskEndTimeCost",
    "RequiredSkillCost",
    "Score",
    "SquaredMakespanCost",
    "UnassignedTasksCost",
    "critical_priority_task_end_time_cost",
    "get_constraints",
    "major_priority_task_end_time_cost",
    "minor_priority_task_end_time_cost",
    "required_skill_cost",
    "score",
    "squared_makespan_cost",
    "unassigned_tasks_cost",
]


class RequiredSkillCost(Constraint):
    """Skills the task type requires that the assigned employee lacks. Default: hard, weight 1.

    hard[0]; the domain's below-hard fold puts one missing skill 10^9 engine-hard
    units above one unassigned task, so the weight is in plain hard units."""

    name = "required_skill_cost"
    label_en = "Required skills"
    label_ko = "필요 기술"
    description_en = (
        "Required skills of each assigned task's type that its employee does not have, summed over "
        "the employee's tasks (one task can miss several). One unit of cost is one missing skill; "
        "0 means every assigned task is done by a fully skilled employee."
    )
    description_ko = (
        "배정된 작업마다 작업 유형이 요구하는 기술 가운데 담당 직원에게 없는 기술을 모두 셉니다(작업 하나에서 "
        "여러 개가 빠질 수 있습니다). 빠진 기술 1개가 비용 1이며, 0이면 모든 작업을 필요한 기술을 다 갖춘 "
        "직원이 맡습니다."
    )


class SquaredMakespanCost(Constraint):
    """The **square** of each employee's end time. Default: soft, weight 10^7.

    Squaring pushes toward taking work off the busiest employee rather than just
    shrinking the total. An employee without tasks ends at 0 and costs nothing. Level
    soft[1]: the default weight 10^7 keeps it above the priority end-time terms on the
    shared soft level.

    The square lives inside cost() rather than in `exponent` on purpose: the exponent
    shapes each unit's raw cost before the weight; here the square is the constraint's
    own definition, so a caller can still shape it further."""

    name = "squared_makespan_cost"
    label_en = "Squared makespan"
    label_ko = "종료 시각 제곱합"
    description_en = (
        "The square of each employee's end time (the end of the last task on the list, in minutes), "
        "summed over the employees, which spreads work away from the busiest employee. One unit of "
        "cost is one squared minute; 0 means no employee has any task."
    )
    description_ko = (
        "직원마다 마지막 작업이 끝나는 시각(분)을 제곱해 모두 더하므로, 가장 바쁜 직원의 일을 다른 직원에게 "
        "나누도록 이끕니다. 분 단위 종료 시각을 제곱한 값 1이 비용 1이며, 0이면 어느 직원에게도 작업이 "
        "없습니다."
    )


class CriticalPriorityTaskEndTimeCost(Constraint):
    """End time of CRITICAL tasks. Default: soft, weight 4 (4x minor)."""

    name = "critical_priority_task_end_time_cost"
    label_en = "Critical task end times"
    label_ko = "긴급 작업 종료 시각"
    description_en = (
        "The end time (minutes) of every CRITICAL-priority task, summed, so critical work is "
        "scheduled early. One unit of cost is one minute; 0 means no CRITICAL task is assigned."
    )
    description_ko = (
        "우선순위가 CRITICAL인 작업마다 끝나는 시각(분)을 모두 더해 긴급한 작업을 앞당기도록 합니다. 1분이 "
        "비용 1이며, 0이면 배정된 CRITICAL 작업이 없습니다."
    )


class MajorPriorityTaskEndTimeCost(Constraint):
    """End time of MAJOR tasks. Default: soft, weight 2 (2x minor)."""

    name = "major_priority_task_end_time_cost"
    label_en = "Major task end times"
    label_ko = "중요 작업 종료 시각"
    description_en = (
        "The end time (minutes) of every MAJOR-priority task, summed, so major work is scheduled "
        "early. One unit of cost is one minute; 0 means no MAJOR task is assigned."
    )
    description_ko = (
        "우선순위가 MAJOR인 작업마다 끝나는 시각(분)을 모두 더해 중요한 작업을 앞당기도록 합니다. 1분이 "
        "비용 1이며, 0이면 배정된 MAJOR 작업이 없습니다."
    )


class MinorPriorityTaskEndTimeCost(Constraint):
    """End time of MINOR tasks. Default: soft, weight 1 — the baseline of the three priority rules."""

    name = "minor_priority_task_end_time_cost"
    label_en = "Minor task end times"
    label_ko = "일반 작업 종료 시각"
    description_en = (
        "The end time (minutes) of every MINOR-priority task, summed: the baseline the critical "
        "and major rules weigh four and two times as heavily. One unit of cost is one minute; 0 "
        "means no MINOR task is assigned."
    )
    description_ko = (
        "우선순위가 MINOR인 작업마다 끝나는 시각(분)을 모두 더합니다. CRITICAL과 MAJOR 규칙은 이 값보다 "
        "각각 네 배, 두 배 무겁게 매깁니다. 1분이 비용 1이며, 0이면 배정된 MINOR 작업이 없습니다."
    )


class UnassignedTasksCost(Constraint):
    """Tasks on no employee's list. Default: medium, weight 1.

    soft[0], the domain's medium level: it folds into the engine's hard level
    **below** the missing-skill count (``engine.hard = hard * 10^9 + medium``), which
    dominates it lexicographically. Unassigned tasks belong to no unit, so
    this is a global constraint."""

    name = "unassigned_tasks_cost"
    label_en = "Unassigned tasks"
    label_ko = "미배정 작업"
    description_en = (
        "The number of tasks on no employee's list, on the medium level (below the missing skills, "
        "above every soft term). One unit of cost is one unassigned task; 0 means every task is assigned."
    )
    description_ko = (
        "어느 직원의 목록에도 들어가지 않은 작업의 수를 미디엄 수준에서 셉니다(이 수준은 빠진 기술보다 낮고 "
        "모든 소프트 항목보다 높습니다). 미배정 작업 1개가 비용 1이며, 0이면 모든 작업이 배정된 상태입니다."
    )


#: Ready-made default instances (``required_skill_cost`` is ``RequiredSkillCost()``).
required_skill_cost = RequiredSkillCost()
squared_makespan_cost = SquaredMakespanCost()
critical_priority_task_end_time_cost = CriticalPriorityTaskEndTimeCost()
major_priority_task_end_time_cost = MajorPriorityTaskEndTimeCost()
minor_priority_task_end_time_cost = MinorPriorityTaskEndTimeCost()
unassigned_tasks_cost = UnassignedTasksCost()

# The default set, in list_constraints() order (unit constraints first, the global after).
_DEFAULTS = (
    required_skill_cost,
    squared_makespan_cost,
    critical_priority_task_end_time_cost,
    major_priority_task_end_time_cost,
    minor_priority_task_end_time_cost,
    unassigned_tasks_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all six default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([RequiredSkillCost(), SquaredMakespanCost(weight=10)])

    Same contract as the C++ applyConstraints: an empty list is an error (pass None
    for the defaults); a constraint named several times is scored once per pick,
    each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `ta.constraint_costs(solution)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent, offset and level rounding),
    its default weight and level, and the domain's medium fold (on every entry).
    Fetch it once and rescore with different weights without re-solving::

        raw = ta.constraint_costs(solution)
        score(raw)                     # default set
        score(raw, [RequiredSkillCost(), SquaredMakespanCost(weight=10)])
        score(raw, [MinorPriorityTaskEndTimeCost(), MinorPriorityTaskEndTimeCost(exponent=2, offset=60)])

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent, offset, clip)`` (a second
    pick of a constraint reads the same raw costs with its own knobs), hard and medium
    contributions are **rounded per unit** before summing, and the medium level
    folds below hard: ``hard * 10^9 + medium`` (even when no medium constraint is
    picked).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft): the hard level carries
    ``hard * 10^9 + soft0`` and the soft level ``soft1 * 10^7 + soft2`` at the default
    weights, so to compare with the split levels of `solution.score`, fold the same way first.
    """
    return score_costs(costs, get_constraints(constraints))
