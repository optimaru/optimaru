"""제약 스팟체크: full recompute (`calculate_score`/`compute_breakdown`) 를
C++ 테스트의 손계산 케이스 일부와 대조한다."""

import sports_league as sl


def make_base(team_count: int, round_count: int, distance=None) -> "sl.LeagueSchedule":
    teams = [sl.Team(str(t)) for t in range(team_count)]
    rounds = [sl.Round(i) for i in range(round_count)]
    return sl.LeagueSchedule(teams, rounds, [], distance or [])


def with_matches(schedule: "sl.LeagueSchedule", matches) -> "sl.LeagueSchedule":
    # 리스트 속성 대입은 통째로만 가능하다 (원소 접근은 복사본).
    return sl.LeagueSchedule(schedule.teams, schedule.rounds, matches, schedule.distance_to_team)


def synthetic_distance(n: int) -> list[list[int]]:
    return [[0 if a == b else 10 * (a + 1) + (b + 1) for b in range(n)] for a in range(n)]


def test_matches_on_same_day():
    s = with_matches(
        make_base(4, 2),
        [sl.Match("a", 0, 1, round=0), sl.Match("b", 0, 1, round=0), sl.Match("c", 0, 1)],
    )
    score = sl.calculate_score(s)
    assert score.hard == -1  # 미배정 세 번째 경기는 기여하지 않는다
    assert score.soft == 0
    assert sl.compute_breakdown(s).matches_on_same_day == 1


def test_consecutive_home_matches():
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
    assert sl.compute_breakdown(s).consecutive_home_matches == 4


def test_repeat_match_on_next_day():
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
    assert sl.compute_breakdown(s).repeat_match_on_next_day == 1


def test_hops():
    # (0,1)@0, (2,0)@1: start d(1,0)=21, homeToAway d(0,2)=13, end d(2,0)=31.
    s = with_matches(
        make_base(4, 2, distance=synthetic_distance(4)),
        [sl.Match("a", 0, 1, round=0), sl.Match("b", 2, 0, round=1)],
    )
    score = sl.calculate_score(s)
    assert score.hard == 0
    assert score.soft == -65
    b = sl.compute_breakdown(s)
    assert b.start_to_away_hop == 21
    assert b.home_to_away_hop == 13
    assert b.away_to_end_hop == 31
    assert b.away_to_away_hop == 0
    assert b.away_to_home_hop == 0


def test_classic_matches():
    teams = [sl.Team(str(t)) for t in range(4)]
    rounds = [sl.Round(0), sl.Round(1, weekend_or_holiday=True)]
    matches = [
        sl.Match("a", 0, 1, classic_match=True, round=0),  # 평일: -1000
        sl.Match("b", 2, 3, classic_match=True, round=1),  # 주말: 0
    ]
    s = sl.LeagueSchedule(teams, rounds, matches)
    score = sl.calculate_score(s)
    assert score.hard == 0
    assert score.soft == -1000
    assert sl.compute_breakdown(s).classic_match_count == 1


def test_demo_schedule_shape():
    s = sl.demo_schedule()
    assert s.team_count() == 14
    assert s.round_count() == 32
    assert s.match_count() == 14 * 13
    classic = sum(1 for m in s.matches if m.classic_match)
    # 9개 추첨 + 대칭 폐포 (java.util.Random(0) 재현으로 결정론).
    assert classic == 18
    assert all(m.round == -1 for m in s.matches)
