"""제약 스팟체크: C++ 테스트의 손계산 케이스를 Python 바인딩으로 재검증.

기대값 유도 (Java TournamentScheduleConstraintProviderTest 시나리오):
  unfairness = round6(sqrt(sum(load^2) - sum(load)^2 / n)),
  soft = -(medium 불공정도 * 1e6 + soft 불공정도).
"""

import pytest

from tournament_scheduling import (
    MEDIUM_SCALE,
    Day,
    Team,
    TeamAssignment,
    TournamentSchedule,
    UnavailabilityPenalty,
    calculate_score,
    unfairness,
)


def make_schedule(assignments, penalties=(), teams=3, days=3):
    return TournamentSchedule(
        [Team(f"T{t}") for t in range(teams)],
        [Day(d) for d in range(days)],
        [UnavailabilityPenalty(t, d) for t, d in penalties],
        [TeamAssignment(i, day, team=team) for i, (day, team) in enumerate(assignments)],
    )


def test_medium_scale():
    assert MEDIUM_SCALE == 1e6


def test_unfairness_spot_values():
    assert unfairness([]) == 0.0
    assert unfairness([5]) == 0.0
    assert unfairness([3, 2]) == pytest.approx(0.707107, abs=1e-12)  # sqrt(1/2)
    assert unfairness([1, 1, 2]) == pytest.approx(0.816497, abs=1e-12)  # sqrt(2/3)
    assert unfairness([11, 11, 10, 10, 10, 10, 10]) == pytest.approx(1.19523, abs=1e-12)  # sqrt(10/7)


def test_one_assignment_per_date_per_team():
    # Java 테스트: T0 이 D0 에 3번 (3쌍), T1 은 D1/D2 (0쌍) -> hard -3.
    # medium: loads {3, 2} -> 0.707107; 대결 없음 (같은 팀 쌍은 제외).
    schedule = make_schedule([(0, 0), (0, 0), (0, 0), (1, 1), (2, 1)])
    score = calculate_score(schedule)
    assert score.hard == -3
    assert score.soft == pytest.approx(-707107.0, abs=1e-6)


def test_unavailability_penalty():
    # Java 테스트: 불가 (T0,D0)/(T1,D1) 둘 다 적중 + T1 이 D1 에 2번 (1쌍).
    schedule = make_schedule(
        [(0, 0), (1, 1), (1, 1), (2, 1)],
        penalties=[(0, 0), (1, 1)],
    )
    score = calculate_score(schedule)
    assert score.hard == -3
    assert score.soft == pytest.approx(-(unfairness([1, 3]) * MEDIUM_SCALE), abs=1e-6)


def test_unavailability_penalty_miss():
    schedule = make_schedule([(0, 0)], penalties=[(2, 0)])
    score = calculate_score(schedule)
    assert score.hard == 0
    assert score.soft == 0.0


def test_fair_assignment_count_per_team():
    # Java 테스트: 팀별 1번씩 -> 완전 공정.
    schedule = make_schedule([(0, 0), (1, 1), (2, 2)])
    score = calculate_score(schedule)
    assert score.hard == 0
    assert score.soft == 0.0
    # T2 만 2번 -> loads {1,1,2} = 0.816497 medium (대결 쌍은 하나라 soft 0).
    schedule = make_schedule([(0, 0), (1, 1), (2, 2), (0, 2)])
    score = calculate_score(schedule)
    assert score.hard == 0
    assert score.soft == pytest.approx(-816497.0, abs=1e-6)


def test_evenly_confrontation_count():
    # Java 테스트: 세 팀이 D0 에서 서로 1번씩 -> 완전 균등.
    schedule = make_schedule([(0, 0), (0, 1), (0, 2)])
    score = calculate_score(schedule)
    assert score.hard == 0
    assert score.soft == 0.0
    # + T2 한 번 더: 대결 (0,1)=1 (0,2)=2 (1,2)=2 -> 0.816497 soft,
    # loads {1,1,2} -> 0.816497 medium, T2 의 같은 날 쌍 -> hard -1.
    schedule = make_schedule([(0, 0), (0, 1), (0, 2), (0, 2)])
    score = calculate_score(schedule)
    assert score.hard == -1
    assert score.soft == pytest.approx(-816497.816497, abs=1e-6)


def test_unassigned_contributes_nothing():
    schedule = make_schedule([(0, -1), (0, 1)], penalties=[(0, 0)])
    score = calculate_score(schedule)
    assert score.hard == 0
    assert score.soft == 0.0
