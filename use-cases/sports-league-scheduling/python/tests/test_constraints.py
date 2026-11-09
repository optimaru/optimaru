"""Constraint spot checks: the full recompute (`calculate_score`) and the raw
per-constraint costs (`constraint_costs`, the only per-constraint surface since 0.2.0)
against some hand-computed cases of the C++ tests."""

import sports_league as sl


def raw_totals(schedule) -> dict[str, float]:
    """Raw cost per constraint name, summed over its units (a global has one total)."""
    return {name: sum(entry["costs"]) for name, entry in sl.constraint_costs(schedule).items()}


def make_base(team_count: int, round_count: int, distance=None) -> "sl.LeagueSchedule":
    teams = [sl.Team(str(t)) for t in range(team_count)]
    rounds = [sl.Round(i) for i in range(round_count)]
    return sl.LeagueSchedule(teams, rounds, [], distance or [])


def with_matches(schedule: "sl.LeagueSchedule", matches) -> "sl.LeagueSchedule":
    # List attributes can only be assigned wholesale (element access returns copies).
    return sl.LeagueSchedule(schedule.teams, schedule.rounds, matches, schedule.distance_to_team)


def synthetic_distance(n: int) -> list[list[int]]:
    return [[0 if a == b else 10 * (a + 1) + (b + 1) for b in range(n)] for a in range(n)]


def test_team_conflict_cost():
    s = with_matches(
        make_base(4, 2),
        [sl.Match("a", 0, 1, round=0), sl.Match("b", 0, 1, round=0), sl.Match("c", 0, 1)],
    )
    score = sl.calculate_score(s)
    assert score.hard == -1  # the unassigned third match contributes nothing
    assert score.soft == 0
    assert raw_totals(s)["team_conflict_cost"] == 1


def test_consecutive_home_matches_cost():
    s = with_matches(
        make_base(4, 5),
        [
            sl.Match("a", 0, 1, round=0),
            sl.Match("b", 0, 2, round=1),
            sl.Match("c", 0, 3, round=2),
            sl.Match("d", 0, 1, round=3),
        ],
    )
    assert sl.calculate_score(s).hard == -4
    assert raw_totals(s)["consecutive_home_matches_cost"] == 4


def test_repeat_match_on_next_day_cost():
    s = with_matches(
        make_base(2, 7),
        [
            sl.Match("a", 0, 1, round=0),
            sl.Match("b", 1, 0, round=1),
            sl.Match("c", 0, 1, round=4),
            sl.Match("d", 1, 0, round=6),
        ],
    )
    assert sl.calculate_score(s).hard == -1
    assert raw_totals(s)["repeat_match_on_next_day_cost"] == 1


def test_hops():
    # (0,1)@0, (2,0)@1: start d(1,0)=21, homeToAway d(0,2)=13, end d(2,0)=31.
    s = with_matches(
        make_base(4, 2, distance=synthetic_distance(4)),
        [sl.Match("a", 0, 1, round=0), sl.Match("b", 2, 0, round=1)],
    )
    score = sl.calculate_score(s)
    assert score.hard == 0
    assert score.soft == -65
    raw = raw_totals(s)
    assert raw["start_to_away_hop_cost"] == 21
    assert raw["home_to_away_hop_cost"] == 13
    assert raw["away_to_end_hop_cost"] == 31
    assert raw["away_to_away_hop_cost"] == 0
    assert raw["away_to_home_hop_cost"] == 0


def test_workday_classic_matches_cost():
    teams = [sl.Team(str(t)) for t in range(4)]
    rounds = [sl.Round(0), sl.Round(1, weekend_or_holiday=True)]
    matches = [
        sl.Match("a", 0, 1, classic_match=True, round=0),  # weekday: -1000
        sl.Match("b", 2, 3, classic_match=True, round=1),  # weekend: 0
    ]
    s = sl.LeagueSchedule(teams, rounds, matches)
    score = sl.calculate_score(s)
    assert score.hard == 0
    assert score.soft == -1000
    assert raw_totals(s)["workday_classic_matches_cost"] == 1
    # One cost per round position: the weekday round 0 holds the one workday derby.
    assert sl.constraint_costs(s)["workday_classic_matches_cost"]["costs"] == [1.0, 0.0]


def test_demo_schedule_shape():
    s = sl.demo_schedule()
    assert s.team_count() == 14
    assert s.round_count() == 32
    assert s.match_count() == 14 * 13
    classic = sum(1 for m in s.matches if m.classic_match)
    # 9 drawn + symmetric closure (deterministic: a seeded draw).
    assert classic == 18
    assert all(m.round == -1 for m in s.matches)
