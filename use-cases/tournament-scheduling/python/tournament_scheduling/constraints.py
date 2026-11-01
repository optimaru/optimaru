"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`ts.solve(schedule)`) enables all four constraints at their default weights.
This module is the alternative path next to it: pick constraints, change weights, change
levels, shape the cost, and pass the list to `solve(schedule, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import tournament_scheduling as ts
    from tournament_scheduling.constraints import (
        ConfrontationCountImbalanceCost, TeamConflictCost, TeamUnavailabilityCost,
        get_constraints,
    )

    ts.solve(schedule, constraints=get_constraints())           # the defaults as-is
    ts.solve(schedule, constraints=[                             # picked
        TeamConflictCost(), TeamUnavailabilityCost(), ConfrontationCountImbalanceCost(weight=10),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``team_conflict_cost`` is
``TeamConflictCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost, in units of its level.
               ``None`` = the C++ component default (``ts.constraint_info()``
               lists them; every default is 1).
- ``level``    ``None`` = the default level, otherwise a ``Level`` or its name
               (``Level.MEDIUM`` or ``"medium"``). Any flippable constraint may
               take any of the three levels. ``AssignmentCountImbalanceCost`` is
               **medium** by default: folded into the engine's soft level as
               ``medium * 1e6`` (``MEDIUM_SCALE``), like any constraint moved onto
               medium. On another level, ``level="hard"`` charges one
               hard unit per raw unit (the global total, rounded),
               ``level="soft"`` one soft unit per raw unit, and a ``weight`` is in
               units of the level in effect.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``, per team for the two hard constraints and on the
               total for the two imbalance constraints. 2.0 = squared, so
               concentrated violations grow much costlier.
- ``offset``   (keyword) shifts that shaping, at the same granularity; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a unit below the offset lowers the
               penalty. The literal form charges every unit the constraint
               evaluates, a raw 0 included: every team for ``TeamConflictCost``,
               only the teams with an unavailability fact for
               ``TeamUnavailabilityCost`` (its ``constraint_costs()`` entry lists
               them as ``"units"``), and the one total for an imbalance.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs and the picks add up. The head-to-head imbalance, plus its square
beyond 2::

    ts.solve(schedule, constraints=[
        *get_constraints(),
        ConfrontationCountImbalanceCost(exponent=2, offset=2),
    ])

``ts.constraint_costs(schedule, picks)`` keys the second pick
``"confrontation_count_imbalance_cost#2"``.

Names (`name`) are the C++ component names in snake_case
(``TeamConflictCost`` -> ``team_conflict_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`ts.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

from ._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "TeamConflictCost",
    "TeamUnavailabilityCost",
    "AssignmentCountImbalanceCost",
    "ConfrontationCountImbalanceCost",
    "team_conflict_cost",
    "team_unavailability_cost",
    "assignment_count_imbalance_cost",
    "confrontation_count_imbalance_cost",
    "get_constraints",
]


class TeamConflictCost(Constraint):
    """Pairs of assignments one team has on the same day (C(c, 2) per team-day cell). Default: hard, weight 1."""

    name = "team_conflict_cost"
    label_en = "Team double-booked on a day"
    label_ko = "같은 날 중복 배정된 팀"
    description_en = (
        "A team given more than one match slot on the same day. Each pair of a team's assignments on one "
        "day costs 1 (c assignments on a day give c(c-1)/2); 0 means every team plays at most once a day."
    )
    description_ko = (
        "한 팀이 같은 날 경기 슬롯을 두 개 이상 받은 경우입니다. 한 팀이 같은 날 받은 배정을 두 개씩 짝지은 "
        "쌍마다 비용 1이며(하루에 c개면 c(c-1)/2), 0이면 모든 팀이 하루에 한 번 이하로 경기합니다."
    )


class TeamUnavailabilityCost(Constraint):
    """(team, day) unavailability facts with at least one assignment on them. Default: hard, weight 1.

    Counted **per fact**, not per assignment — two facts in one cell give 2, three
    assignments on one fact still give 1 (Java's ifExists semantics)."""

    name = "team_unavailability_cost"
    label_en = "Team plays on an unavailable day"
    label_ko = "출전할 수 없는 날의 경기"
    description_en = (
        "A team scheduled on a day it cannot play. Each unavailability fact whose team holds at least one "
        "assignment on that day costs 1, however many assignments it holds there; 0 means no team plays on "
        "an unavailable day."
    )
    description_ko = (
        "팀이 출전할 수 없는 날에 경기가 잡힌 경우입니다. 출전 불가 일정마다, 그날 그 팀에 배정이 하나라도 "
        "있으면 배정 수와 상관없이 비용 1이며, 0이면 어떤 팀도 출전할 수 없는 날에 경기하지 않습니다."
    )


class AssignmentCountImbalanceCost(Constraint):
    """LoadBalance unfairness of assignment counts per team. Default: medium, weight 1.

    Only teams that actually have assignments enter the balance (the Java rule has no
    complement). The statistic crosses units, so this is a global constraint. The weight
    is in medium units; the domain folds the medium level into the engine soft level
    as `medium * 1e6`."""

    name = "assignment_count_imbalance_cost"
    label_en = "Uneven assignment counts across teams"
    label_ko = "팀별 배정 수 불균형"
    description_en = (
        "How unevenly the match slots are spread over the teams that play, on the medium level: "
        "sqrt(sum(count^2) - sum(count)^2 / n) over the n teams with at least one assignment, rounded to "
        "6 significant figures. One unit of cost is one unit of that spread; 0 means every playing team "
        "holds the same number of assignments."
    )
    description_ko = (
        "경기에 나서는 팀들 사이에 경기 슬롯이 얼마나 고르지 않게 나뉘었는지를 미디엄 수준에서 매깁니다. "
        "배정이 하나 이상인 n개 팀의 배정 수로 sqrt(제곱합 - 합의 제곱 / n)을 유효숫자 6자리로 계산합니다. "
        "이 값이 그대로 비용이며, 0이면 경기하는 모든 팀의 배정 수가 같습니다."
    )


class ConfrontationCountImbalanceCost(Constraint):
    """LoadBalance unfairness of head-to-head counts per team pair. Default: soft, weight 1.

    Each pair of same-day assignments of two different teams counts as one head-to-head
    (a team is not paired with itself), and only pairs with at least one head-to-head
    enter the balance. The statistic is over team **pairs**, so it is a global
    constraint no unit owns."""

    name = "confrontation_count_imbalance_cost"
    label_en = "Uneven head-to-head counts across team pairs"
    label_ko = "팀 쌍별 맞대결 수 불균형"
    description_en = (
        "How unevenly the head-to-head meetings are spread over the team pairs that meet: "
        "sqrt(sum(count^2) - sum(count)^2 / n) over the n pairs with at least one same-day meeting, "
        "rounded to 6 significant figures. One unit of cost is one unit of that spread; 0 means every "
        "meeting pair meets equally often."
    )
    description_ko = (
        "맞붙는 팀 쌍들 사이에 맞대결이 얼마나 고르지 않게 나뉘었는지를 매깁니다. 같은 날 한 번 이상 만나는 "
        "n개 팀 쌍의 맞대결 수로 sqrt(제곱합 - 합의 제곱 / n)을 유효숫자 6자리로 계산합니다. 이 값이 "
        "그대로 비용이며, 0이면 맞붙는 모든 팀 쌍이 같은 횟수만큼 만납니다."
    )


#: Ready-made default instances (``team_conflict_cost`` is ``TeamConflictCost()``).
team_conflict_cost = TeamConflictCost()
team_unavailability_cost = TeamUnavailabilityCost()
assignment_count_imbalance_cost = AssignmentCountImbalanceCost()
confrontation_count_imbalance_cost = ConfrontationCountImbalanceCost()

# The default set, in list_constraints() order (unit constraints first, globals after).
_DEFAULTS = (
    team_conflict_cost,
    team_unavailability_cost,
    assignment_count_imbalance_cost,
    confrontation_count_imbalance_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all four default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([TeamConflictCost(), ConfrontationCountImbalanceCost(weight=10)])

    Same contract as the C++ applyConstraints: an empty list is an error (pass
    None for the defaults); a constraint named several times is scored once per
    pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `ts.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent, offset and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = ts.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, [TeamConflictCost(), ConfrontationCountImbalanceCost(weight=10)])
        score(raw, [TeamConflictCost(), TeamConflictCost(exponent=2, offset=1)])

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent, offset, clip)`` (a
    second pick of a constraint reads the same raw costs with its own knobs) and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft) — the same as
    `schedule.score`: the medium level (`assignment_count_imbalance_cost`) is
    folded into soft as ``medium * 1e6``, the ``medium_fold`` every entry of
    ``costs`` carries.
    """
    return score_costs(costs, get_constraints(constraints))
