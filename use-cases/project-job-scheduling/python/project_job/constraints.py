"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`pjs.solve(schedule)`) enables all four constraints at the default weights.
This module is the alternative path next to it: pick constraints, change weights, change
levels, shape the cost, and pass the list to `solve(schedule, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import project_job as pjs
    from project_job.constraints import (
        MakespanCost, ProjectDelayCost, RenewableResourceCapacityCost, get_constraints,
    )

    pjs.solve(schedule, constraints=get_constraints())          # the defaults as-is
    pjs.solve(schedule, constraints=[                            # picked
        RenewableResourceCapacityCost(), ProjectDelayCost(), MakespanCost(weight=10),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``makespan_cost`` is
``MakespanCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost, in units of its level.
               ``None`` = the component default (``pjs.constraint_info()`` reports
               them).
- ``level``    ``None`` = the default level, otherwise a ``Level`` or its name
               (``Level.MEDIUM`` or ``"medium"``). Any flippable constraint may
               take any of the three levels. ``ProjectDelayCost`` is a MEDIUM
               constraint by default: ``level="hard"`` makes it one hard unit per
               day and ``level="soft"`` one soft unit per day (outside the 10^9
               medium fold), in the solver and in :func:`score` alike. A
               ``weight`` is in units of the level in effect.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. All four constraints are global, so it shapes each one's
               single total. 2.0 = squared, so concentrated violations grow much
               costlier.
- ``offset``   (keyword) shifts that shaping of the total; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a total below the offset lowers the
               penalty (a makespan under a target day is rewarded).

The same constraint may be picked several times; each pick is scored on its own with
its own knobs and the picks add up. The makespan, plus the square of the days beyond
day 40::

    pjs.solve(schedule, constraints=[
        *get_constraints(),
        MakespanCost(exponent=2, offset=40),
    ])

``pjs.constraint_costs(schedule, picks)`` keys the second pick ``"makespan_cost#2"``.

Names (`name`) are the C++ component names in snake_case
(``ProjectDelayCost`` -> ``project_delay_cost``). The core derives them mechanically,
so there is no hand-maintained alias table — check them with `pjs.list_constraints()`.
Module-level instance names use the same snake_case, so they match the strings
`list_constraints()` prints.
"""

from ._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "NonRenewableResourceCapacityCost",
    "RenewableResourceCapacityCost",
    "ProjectDelayCost",
    "MakespanCost",
    "non_renewable_resource_capacity_cost",
    "renewable_resource_capacity_cost",
    "project_delay_cost",
    "makespan_cost",
    "get_constraints",
]


class NonRenewableResourceCapacityCost(Constraint):
    """Total demand on a non-renewable resource above its capacity. Default: hard, weight 1.

    Stock that is gone once used, so "when" things run does not matter — the only
    component that does not read the date chain."""

    name = "non_renewable_resource_capacity_cost"
    label_en = "Non-renewable resource over capacity"
    label_ko = "비재생 자원 용량 초과"
    description_en = (
        "For every non-renewable resource (a stock that is used up, such as a budget), the total "
        "requirement of the execution modes chosen for all jobs beyond its capacity, summed over "
        "those resources; when the jobs run does not matter. One unit of cost is one resource unit "
        "over capacity; 0 means every stock suffices."
    )
    description_ko = (
        "비재생 자원(예산처럼 한 번 쓰면 없어지는 자원)마다 각 작업에 선택된 실행 모드의 요구량 합계가 "
        "용량을 넘는 양을 모두 더합니다. 작업을 언제 하는지는 상관없습니다. 용량을 넘은 자원 1단위가 "
        "비용 1이며, 0이면 모든 비재생 자원이 용량 안에 있습니다."
    )


class RenewableResourceCapacityCost(Constraint):
    """Per (renewable resource, date), total demand of allocations busy that day above
    capacity. Default: hard, weight 1.

    Capacity that recovers daily, so it is purely a question of "when" things run, and
    it reads the whole date chain."""

    name = "renewable_resource_capacity_cost"
    label_en = "Renewable resource over daily capacity"
    label_ko = "재생 자원 일별 용량 초과"
    description_en = (
        "For every renewable resource (capacity that is available again each day, such as people "
        "or machines) and every day, the requirement of the jobs running that day beyond its "
        "capacity, summed over resources and days. One unit of cost is one resource unit over "
        "capacity on one day; 0 means no resource is overbooked on any day."
    )
    description_ko = (
        "재생 자원(사람이나 기계처럼 날마다 다시 쓸 수 있는 자원)과 날짜마다 그날 진행 중인 작업들의 "
        "요구량 합계가 용량을 넘는 양을 자원과 날짜에 걸쳐 모두 더합니다. 하루 동안 용량을 넘은 자원 1단위가 "
        "비용 1이며, 0이면 어느 날에도 용량을 넘는 자원이 없습니다."
    )


class ProjectDelayCost(Constraint):
    """Days by which each project's SINK passes its critical path end. Default: medium,
    weight 1 (medium units).

    Measured from (releaseDate + criticalPathDuration); finishing early costs nothing —
    only delay counts. The domain folds medium into the engine soft level as
    ``medium * 10^9``; the fold belongs to the domain (every ``constraint_costs()`` entry
    carries it as ``medium_fold``), so the weight carries no 10^9 factor."""

    name = "project_delay_cost"
    label_en = "Project delay"
    label_ko = "프로젝트 지연"
    description_en = (
        "Days by which each project's last job (its sink) ends after the project's critical-path "
        "end date (release date + critical path duration), summed over the projects; finishing "
        "early earns nothing. One unit of cost is one day of delay; 0 means every project ends "
        "on time."
    )
    description_ko = (
        "프로젝트마다 마지막 작업(싱크)이 끝나는 날이 임계 경로 종료일보다 늦은 일수를 모두 더합니다. "
        "임계 경로 종료일은 프로젝트의 착수 가능일(릴리스일)에 임계 경로 기간을 더한 날입니다. 일찍 "
        "끝나도 이득은 없습니다. 하루 지연이 비용 1이며, 0이면 모든 프로젝트가 제때 끝납니다."
    )


class MakespanCost(Constraint):
    """Latest SINK end date — when the last project finishes. Default: soft, weight 1.

    One penalty for the whole schedule, not per project (Java's groupBy(max)), so
    pulling in a project that is not last is free in this term. The value is a
    day index counted from day 0 (release dates do not move it), not a duration."""

    name = "makespan_cost"
    label_en = "Makespan"
    label_ko = "전체 완료일(메이크스팬)"
    description_en = (
        "The day the last project finishes: the latest end date of any project's last job (its "
        "sink), as a day index counted from day 0 (release dates do not shift the schedule; they "
        "only set the project-delay reference). One value for the whole schedule, so only the "
        "project that finishes last moves it. One unit of cost is one day; 0 means every project "
        "ends on day 0."
    )
    description_ko = (
        "가장 늦게 끝나는 프로젝트의 종료일, 곧 모든 프로젝트의 마지막 작업(싱크) 가운데 가장 늦은 "
        "종료일을 0일부터 센 날짜 번호로 나타냅니다(착수 가능일은 일정을 늦추지 않고 프로젝트 지연의 "
        "기준일에만 쓰입니다). 일정 전체에 값이 하나뿐이어서 가장 늦게 끝나는 프로젝트만 이 값을 "
        "바꿉니다. 하루가 비용 1이며, 0이면 모든 프로젝트가 0일에 끝납니다."
    )


#: Ready-made default instances (``makespan_cost`` is ``MakespanCost()``).
non_renewable_resource_capacity_cost = NonRenewableResourceCapacityCost()
renewable_resource_capacity_cost = RenewableResourceCapacityCost()
project_delay_cost = ProjectDelayCost()
makespan_cost = MakespanCost()

#: Default set — same order as `pjs.list_constraints()`.
_DEFAULTS = (
    non_renewable_resource_capacity_cost,
    renewable_resource_capacity_cost,
    project_delay_cost,
    makespan_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all four default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([RenewableResourceCapacityCost(), MakespanCost(weight=10)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) raises ValueError; a constraint named several times is
    scored once per pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `pjs.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent, offset and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = pjs.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, [RenewableResourceCapacityCost(), MakespanCost(weight=10)])
        score(raw, [MakespanCost(), MakespanCost(exponent=2, offset=40)])

    No solver and no search are involved. The arithmetic matches the engine —
    each total contributes ``weight * shape(cost, exponent, offset, clip)`` and
    hard contributions are **rounded per unit** before summing (the same order as
    the engine; rounding the total afterwards gives different values). A second
    pick of a constraint reads the same raw costs with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft): the domain's medium
    fold (``medium_fold`` on every entry, into soft at 10^9) is applied, so it
    equals ``(score.hard, score.medium * 10**9 + score.soft)`` of a schedule scored
    by the same constraints.
    """
    return score_costs(costs, get_constraints(constraints))
