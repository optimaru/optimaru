"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`mnt.solve(schedule)`) enables all six constraints at their default
weights (the quickstart's). This module is the alternative path next to it: pick
constraints, change weights, change levels, shape the cost, and pass the list to
`solve(schedule, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import maintenance_scheduling as mnt
    from maintenance_scheduling.constraints import (
        CrewConflictCost, TagConflictCost, get_constraints,
    )

    mnt.solve(schedule, constraints=get_constraints())          # the defaults as-is
    mnt.solve(schedule, constraints=[                           # picked
        CrewConflictCost(), TagConflictCost(weight=10_000),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``crew_conflict_cost``
is ``CrewConflictCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (MinStartDateCost, MaxEndDateCost, CrewConflictCost and
               BeforeIdealEndDateCost 1, TagConflictCost 1000,
               AfterIdealEndDateCost 1'000'000); ``mnt.constraint_info()`` reports them.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level: ``level="medium"`` raises ValueError.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. 2.0 = squared, so concentrated violations grow much
               costlier. The four date rules shape per (start date, crew) cell; the
               two global conflict rules shape their single total.
- ``offset``   (keyword) shifts that shaping, at the same granularity; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a cell below the offset — an empty one
               (cost 0) included, every crew on every start date — lowers the
               penalty, and a global total below it does too.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs (its level included) and the picks add up. A tag-conflict day costs 1000, and the total
beyond ten days costs its square on top::

    mnt.solve(schedule, constraints=[
        *get_constraints(),
        TagConflictCost(weight=100, exponent=2, offset=10),
    ])

``mnt.constraint_costs(schedule, picks)`` keys the second pick ``"tag_conflict_cost#2"``.

The weight ratio of the three soft terms (1 : 1'000 : 1'000'000) is the character of this
domain — lateness is a thousand times a tag conflict and a million times early completion.

Names (`name`) are the C++ component names in snake_case
(``CrewConflictCost`` -> ``crew_conflict_cost``). The core derives them mechanically,
so there is no hand-maintained alias table — check them with `mnt.list_constraints()`.
Module-level instance names use the same snake_case, so they match the strings
`list_constraints()` prints. (The ``_cost`` suffix also keeps the constraint names
apart from the job fields ``min_start_date`` / ``max_end_date`` they read.)
"""

from ._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "MinStartDateCost",
    "MaxEndDateCost",
    "BeforeIdealEndDateCost",
    "AfterIdealEndDateCost",
    "CrewConflictCost",
    "TagConflictCost",
    "min_start_date_cost",
    "max_end_date_cost",
    "before_ideal_end_date_cost",
    "after_ideal_end_date_cost",
    "crew_conflict_cost",
    "tag_conflict_cost",
    "get_constraints",
]


class MinStartDateCost(Constraint):
    """Calendar days a job starts before its earliest start date. Default: hard, weight 1."""

    name = "min_start_date_cost"
    label_en = "Earliest start date"
    label_ko = "시작 가능일"
    description_en = (
        "Calendar days by which an assigned job starts before its earliest allowed start date "
        "(min_start_date), summed over the jobs. One unit of cost is one day too early; 0 means "
        "every job starts on or after its earliest start date."
    )
    description_ko = (
        "배정된 작업마다 시작일이 시작 가능일(min_start_date)보다 앞선 날수를 모두 더합니다. "
        "하루 일찍 시작할 때마다 비용 1이며, 0이면 모든 작업이 시작 가능일 당일이나 그 뒤에 시작합니다."
    )


class MaxEndDateCost(Constraint):
    """Calendar days a job ends after its end deadline. Default: hard, weight 1."""

    name = "max_end_date_cost"
    label_en = "Latest end date"
    label_ko = "종료 기한"
    description_en = (
        "Calendar days by which an assigned job ends after the latest date it may end on "
        "(max_end_date), summed over the jobs. One unit of cost is one day too late; 0 means "
        "every job ends by its latest end date."
    )
    description_ko = (
        "배정된 작업마다 종료일이 종료 기한(max_end_date)을 넘긴 날수를 모두 더합니다. "
        "하루 늦게 끝날 때마다 비용 1이며, 0이면 모든 작업이 종료 기한 안에 끝납니다."
    )


class BeforeIdealEndDateCost(Constraint):
    """Calendar days a job ends **before** its ideal end date. Default: soft, weight 1.

    The cheapest of the three soft terms by design — bringing maintenance forward
    throws away remaining service life, but that loss is light."""

    name = "before_ideal_end_date_cost"
    label_en = "Finished before the ideal date"
    label_ko = "이상적 종료일보다 이른 완료"
    description_en = (
        "Calendar days by which an assigned job ends before its ideal end date (ideal_end_date), "
        "summed over the jobs. One unit of cost is one day early; 0 means no job ends early."
    )
    description_ko = (
        "배정된 작업마다 이상적 종료일(ideal_end_date)보다 일찍 끝난 날수를 모두 더합니다. "
        "하루 일찍 끝날 때마다 비용 1이며, 0이면 일찍 끝나는 작업이 없습니다."
    )


class AfterIdealEndDateCost(Constraint):
    """Calendar days a job ends **after** its ideal end date. Default: soft, weight 10^6.

    A thousand times a tag conflict and a million times early completion — which is why
    this is the soft term the search clears first."""

    name = "after_ideal_end_date_cost"
    label_en = "Finished after the ideal date"
    label_ko = "이상적 종료일보다 늦은 완료"
    description_en = (
        "Calendar days by which an assigned job ends after its ideal end date (ideal_end_date), "
        "summed over the jobs. One unit of cost is one day late; 0 means no job ends late."
    )
    description_ko = (
        "배정된 작업마다 이상적 종료일(ideal_end_date)보다 늦게 끝난 날수를 모두 더합니다. "
        "하루 늦게 끝날 때마다 비용 1이며, 0이면 늦게 끝나는 작업이 없습니다."
    )


class CrewConflictCost(Constraint):
    """Calendar days two jobs of the same crew overlap. Default: hard, weight 1.

    Each crew owns one row, so a job occupies exactly one row for the days it runs.
    Two jobs of the same crew can sit in different (start date, crew) cells, so this is
    a global constraint."""

    name = "crew_conflict_cost"
    label_en = "Crew conflict"
    label_ko = "정비조 중복 배정"
    description_en = (
        "Calendar days on which two jobs of the same crew overlap, summed over every pair of "
        "jobs on one crew. One unit of cost is one day a pair of same-crew jobs runs at once; "
        "0 means no crew has two jobs on the same day."
    )
    description_ko = (
        "같은 정비조에 배정된 두 작업이 겹치는 날수를 작업 쌍마다 모두 더합니다. "
        "같은 정비조의 두 작업이 하루 겹칠 때마다 비용 1이며, 0이면 어느 정비조도 같은 날 두 작업을 맡지 않습니다."
    )


class TagConflictCost(Constraint):
    """Calendar days two jobs sharing a tag overlap. Default: soft, weight 1000.

    Counted once **per shared tag**. Each tag owns one row, so a job occupies as many
    rows as it carries tags. Global for the same reason as `crew_conflict_cost`."""

    name = "tag_conflict_cost"
    label_en = "Tag conflict"
    label_ko = "태그 충돌"
    description_en = (
        "Calendar days on which two jobs that share a tag overlap, counted once per shared tag "
        "and summed over every pair of jobs. One unit of cost is one day two jobs with a common "
        "tag run at once (two shared tags count twice); 0 means jobs with a common tag never "
        "run on the same day."
    )
    description_ko = (
        "태그를 공유하는 두 작업이 겹치는 날수를 공유 태그마다 한 번씩 세어 작업 쌍마다 모두 더합니다. "
        "공통 태그가 있는 두 작업이 하루 겹칠 때마다 비용 1(공유 태그가 둘이면 2)이며, "
        "0이면 공통 태그가 있는 작업끼리 같은 날 진행되지 않습니다."
    )


#: Ready-made default instances (``crew_conflict_cost`` is ``CrewConflictCost()``).
min_start_date_cost = MinStartDateCost()
max_end_date_cost = MaxEndDateCost()
before_ideal_end_date_cost = BeforeIdealEndDateCost()
after_ideal_end_date_cost = AfterIdealEndDateCost()
crew_conflict_cost = CrewConflictCost()
tag_conflict_cost = TagConflictCost()

# The default set, in list_constraints() order (unit constraints first, globals after).
_DEFAULTS = (
    min_start_date_cost,
    max_end_date_cost,
    before_ideal_end_date_cost,
    after_ideal_end_date_cost,
    crew_conflict_cost,
    tag_conflict_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all six default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([CrewConflictCost(), TagConflictCost(weight=10_000)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) raises ValueError; a constraint named several times is
    scored once per pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `mnt.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent, offset and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = mnt.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, [CrewConflictCost(), TagConflictCost(weight=10_000)])
        score(raw, [TagConflictCost(), TagConflictCost(exponent=2, offset=10)])

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent, offset, clip)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values). A
    second pick of a constraint reads the same raw costs with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    return score_costs(costs, get_constraints(constraints))
