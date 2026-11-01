"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`sl.solve(schedule)`) enables all ten constraints at their default
weights. This module is the alternative path next to it: pick constraints, change
weights, change levels, shape the cost, and pass the list to
`solve(schedule, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import sports_league as sl
    from sports_league.constraints import (
        TeamConflictCost, WorkdayClassicMatchesCost, get_constraints,
    )

    sl.solve(schedule, constraints=get_constraints())          # the defaults as-is
    sl.solve(schedule, constraints=[                           # picked
        TeamConflictCost(), WorkdayClassicMatchesCost(weight=10_000),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``team_conflict_cost``
is ``TeamConflictCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (1 for every constraint but WorkdayClassicMatchesCost,
               whose default is 1000). ``sl.constraint_info()`` reports the defaults.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level: ``level="medium"`` raises ValueError.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. 2.0 = squared, so concentrated violations grow much
               costlier. The three unit constraints (StartToAwayHopCost,
               AwayToEndHopCost, WorkdayClassicMatchesCost) shape the cost of each
               round; the seven global constraints shape their one total.
- ``offset``   (keyword) shifts that shaping, at the same granularity; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a round below the offset — an empty one
               (cost 0) included — lowers the penalty, and a global total below it
               does too.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs (its level included) and the picks add up. Every away-to-away kilometre costs 1, and the
total beyond 1000 km costs 0.01 x its square on top::

    sl.solve(schedule, constraints=[
        *get_constraints(),
        AwayToAwayHopCost(weight=0.01, exponent=2, offset=1000),
    ])

``sl.constraint_costs(schedule, picks)`` keys the second pick ``"away_to_away_hop_cost#2"``.

The five travel terms (`*_hop_cost`) are all in km, so adding them up makes sense.
The 1'000 of `workday_classic_matches_cost` is set against those km — expensive enough
that the search takes long detours to put a derby on a Saturday.

Names (`name`) are the C++ component names in snake_case
(``TeamConflictCost`` -> ``team_conflict_cost``). The core derives them mechanically,
so there is no hand-maintained alias table — check them with `sl.list_constraints()`.
Module-level instance names use the same snake_case, so they match the strings
`list_constraints()` prints.
"""

from ._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "StartToAwayHopCost",
    "AwayToEndHopCost",
    "WorkdayClassicMatchesCost",
    "TeamConflictCost",
    "ConsecutiveHomeMatchesCost",
    "ConsecutiveAwayMatchesCost",
    "RepeatMatchOnNextDayCost",
    "HomeToAwayHopCost",
    "AwayToAwayHopCost",
    "AwayToHomeHopCost",
    "start_to_away_hop_cost",
    "away_to_end_hop_cost",
    "workday_classic_matches_cost",
    "team_conflict_cost",
    "consecutive_home_matches_cost",
    "consecutive_away_matches_cost",
    "repeat_match_on_next_day_cost",
    "home_to_away_hop_cost",
    "away_to_away_hop_cost",
    "away_to_home_hop_cost",
    "get_constraints",
]


class TeamConflictCost(Constraint):
    """Pairs of matches in the same round sharing at least one team. Default: hard, weight 1.

    A pair sharing **both** teams (a fixture and its return) also counts once."""

    name = "team_conflict_cost"
    label_en = "Team conflict"
    label_ko = "팀 일정 충돌"
    description_en = (
        "Pairs of matches in the same round that share a team; a pair sharing both teams counts once. "
        "One unit of cost is one such pair; 0 means no team plays twice in a round."
    )
    description_ko = (
        "같은 라운드에 열리면서 한 팀 이상이 겹치는 경기 쌍의 수이며, 두 팀이 모두 겹치는 쌍도 한 번만 셉니다. "
        "그런 쌍 하나가 비용 1이며, 0이면 한 라운드에 두 번 경기하는 팀이 없습니다."
    )


class ConsecutiveHomeMatchesCost(Constraint):
    """Teams playing at home four or more rounds in a row. Default: hard, weight 1.

    The cost is the whole run length (a run of 4 costs 4, not the excess over 3).
    Several home matches in one round count once — what makes the run is the round
    being there at all."""

    name = "consecutive_home_matches_cost"
    label_en = "Consecutive home matches"
    label_ko = "연속 홈 경기"
    description_en = (
        "For each team, the full length of every run of four or more consecutive rounds played at home "
        "(a run of 4 costs 4). One unit of cost is one round of such a run; "
        "0 means no team plays at home more than three rounds in a row."
    )
    description_ko = (
        "팀마다 네 라운드 이상 연속으로 홈 경기를 치르는 구간의 길이를 모두 더합니다(4연속이면 4). "
        "그런 구간의 라운드 하나가 비용 1이며, 0이면 어느 팀도 세 라운드를 넘게 연속으로 홈 경기를 치르지 않습니다."
    )


class ConsecutiveAwayMatchesCost(Constraint):
    """The away mirror of `consecutive_home_matches_cost`. Default: hard, weight 1.

    This is the one that actually keeps teams from living on the road for weeks."""

    name = "consecutive_away_matches_cost"
    label_en = "Consecutive away matches"
    label_ko = "연속 원정 경기"
    description_en = (
        "For each team, the full length of every run of four or more consecutive rounds played away "
        "(a run of 4 costs 4). One unit of cost is one round of such a run; "
        "0 means no team plays away more than three rounds in a row."
    )
    description_ko = (
        "팀마다 네 라운드 이상 연속으로 원정 경기를 치르는 구간의 길이를 모두 더합니다(4연속이면 4). "
        "그런 구간의 라운드 하나가 비용 1이며, 0이면 어느 팀도 세 라운드를 넘게 연속으로 원정 경기를 치르지 않습니다."
    )


class RepeatMatchOnNextDayCost(Constraint):
    """Matches whose return fixture is played in the very next round. Default: hard, weight 1.

    At most 1 point per match however many return fixtures are there (Java's ifExists)."""

    name = "repeat_match_on_next_day_cost"
    label_en = "Return match in the next round"
    label_ko = "다음 라운드 리턴 매치"
    description_en = (
        "Matches whose return fixture (home and away swapped) is played in the very next round, "
        "counted at most once per match. One unit of cost is one such match; "
        "0 means no return fixture follows immediately."
    )
    description_ko = (
        "홈과 원정을 바꾼 리턴 매치가 바로 다음 라운드에 열리는 경기의 수이며, 경기마다 한 번만 셉니다. "
        "그런 경기 하나가 비용 1이며, 0이면 리턴 매치가 곧바로 이어지는 경우가 없습니다."
    )


class StartToAwayHopCost(Constraint):
    """Km the away team travels from its home to a match in a round with no round index
    right before it (the season's **first** round, or the first round after a gap in the
    indices). Default: soft, weight 1."""

    name = "start_to_away_hop_cost"
    label_en = "Travel to the first round"
    label_ko = "첫 라운드 원정 이동"
    description_en = (
        "Kilometres each away team travels from its home to a match in a round with no round index "
        "right before it (the season's first round, or the first round after a gap in the indices). "
        "One unit of cost is one kilometre; 0 means no such trip."
    )
    description_ko = (
        "바로 앞 라운드 번호가 없는 라운드(시즌 첫 라운드, 또는 라운드 번호가 비는 구간 바로 뒤의 라운드)의 "
        "경기마다 원정 팀이 연고지에서 경기장까지 이동하는 거리(km)를 더합니다. "
        "1km가 비용 1이며, 0이면 그런 이동이 없습니다."
    )


class HomeToAwayHopCost(Constraint):
    """Km traveled by a team going away in the round after a home match. Default: soft, weight 1.

    For pairs (m1 in round i, m2 in i+1) with m2.away == m1.home,
    dist(m1.home, m2.home) is charged."""

    name = "home_to_away_hop_cost"
    label_en = "Home-to-away travel"
    label_ko = "홈에서 원정으로 이동"
    description_en = (
        "Kilometres a team travels from its home ground to the next host "
        "when it plays at home in one round and away in the next. "
        "One unit of cost is one kilometre; 0 means no such trip."
    )
    description_ko = (
        "한 라운드를 홈에서 치르고 다음 라운드를 원정으로 치르는 팀이 연고지에서 다음 상대의 연고지까지 "
        "이동하는 거리(km)를 더합니다. 1km가 비용 1이며, 0이면 그런 이동이 없습니다."
    )


class AwayToAwayHopCost(Constraint):
    """Km a team on two consecutive away rounds travels from one home ground to the next.
    Default: soft, weight 1."""

    name = "away_to_away_hop_cost"
    label_en = "Away-to-away travel"
    label_ko = "원정에서 원정으로 이동"
    description_en = (
        "Kilometres a team playing away in two consecutive rounds travels straight from one host's ground "
        "to the next. One unit of cost is one kilometre; 0 means no such trip."
    )
    description_ko = (
        "두 라운드 연속으로 원정을 치르는 팀이 한 상대의 연고지에서 다음 상대의 연고지로 곧장 이동하는 "
        "거리(km)를 더합니다. 1km가 비용 1이며, 0이면 그런 이동이 없습니다."
    )


class AwayToHomeHopCost(Constraint):
    """Km a team playing at home after an away round travels back. Default: soft, weight 1.

    The distance is dist(m1.home, m1.away) of the **earlier** match — it depends only on
    m1, so the later match contributes the earlier match's distance, not its own."""

    name = "away_to_home_hop_cost"
    label_en = "Away-to-home travel"
    label_ko = "원정에서 홈으로 이동"
    description_en = (
        "Kilometres a team travels back home when it plays away in one round and at home in the next "
        "(the distance of the earlier away match). One unit of cost is one kilometre; 0 means no such trip."
    )
    description_ko = (
        "한 라운드를 원정으로 치르고 다음 라운드를 홈에서 치르는 팀이 연고지로 돌아오는 거리(km, 앞선 원정 "
        "경기의 거리)를 더합니다. 1km가 비용 1이며, 0이면 그런 이동이 없습니다."
    )


class AwayToEndHopCost(Constraint):
    """Km the away team travels home after a match in a round with no round index right
    after it (the season's **last** round, or the last round before a gap in the indices).
    Default: soft, weight 1."""

    name = "away_to_end_hop_cost"
    label_en = "Travel home after the last round"
    label_ko = "마지막 라운드 후 귀환 이동"
    description_en = (
        "Kilometres each away team travels home after a match in a round with no round index "
        "right after it (the season's last round, or the last round before a gap in the indices). "
        "One unit of cost is one kilometre; 0 means no such trip."
    )
    description_ko = (
        "바로 다음 라운드 번호가 없는 라운드(시즌 마지막 라운드, 또는 라운드 번호가 비는 구간 바로 앞의 "
        "라운드)의 경기마다 원정 팀이 연고지로 돌아가는 거리(km)를 더합니다. "
        "1km가 비용 1이며, 0이면 그런 이동이 없습니다."
    )


class WorkdayClassicMatchesCost(Constraint):
    """Derbies played in a round that is neither a weekend nor a holiday. Default: soft, weight 1000.

    Expensive enough against the km travel terms that the search takes long detours
    to put a derby on a Saturday."""

    name = "workday_classic_matches_cost"
    label_en = "Derby on a workday"
    label_ko = "평일 더비 경기"
    description_en = (
        "Classic matches (derbies) played in a round that is neither a weekend nor a holiday. "
        "One unit of cost is one such derby (default weight 1000); "
        "0 means every derby is on a weekend or holiday."
    )
    description_ko = (
        "주말도 공휴일도 아닌 라운드에 열리는 더비(클래식 매치)의 수입니다. "
        "그런 더비 하나가 비용 1(기본 가중치 1000)이며, 0이면 모든 더비가 주말이나 공휴일에 열립니다."
    )


#: Ready-made default instances (``team_conflict_cost`` is ``TeamConflictCost()``).
start_to_away_hop_cost = StartToAwayHopCost()
away_to_end_hop_cost = AwayToEndHopCost()
workday_classic_matches_cost = WorkdayClassicMatchesCost()
team_conflict_cost = TeamConflictCost()
consecutive_home_matches_cost = ConsecutiveHomeMatchesCost()
consecutive_away_matches_cost = ConsecutiveAwayMatchesCost()
repeat_match_on_next_day_cost = RepeatMatchOnNextDayCost()
home_to_away_hop_cost = HomeToAwayHopCost()
away_to_away_hop_cost = AwayToAwayHopCost()
away_to_home_hop_cost = AwayToHomeHopCost()

# The default set, in list_constraints() order (unit constraints first, globals after).
_DEFAULTS = (
    start_to_away_hop_cost,
    away_to_end_hop_cost,
    workday_classic_matches_cost,
    team_conflict_cost,
    consecutive_home_matches_cost,
    consecutive_away_matches_cost,
    repeat_match_on_next_day_cost,
    home_to_away_hop_cost,
    away_to_away_hop_cost,
    away_to_home_hop_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all ten default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([TeamConflictCost(), WorkdayClassicMatchesCost(weight=10_000)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) raises ValueError; a constraint named several times is
    scored once per pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `sl.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent, offset and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = sl.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, [TeamConflictCost(), WorkdayClassicMatchesCost(weight=10_000)])
        score(raw, [AwayToAwayHopCost(), AwayToAwayHopCost(exponent=2, offset=1000)])

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent, offset, clip)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values).
    ``weight=None`` / ``level=None`` read the entry's default weight and level. A
    second pick of a constraint reads the same raw costs with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    return score_costs(costs, get_constraints(constraints))
