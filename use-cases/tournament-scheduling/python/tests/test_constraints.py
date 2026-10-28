"""Constraint spot checks: re-verifies the hand-computed cases of the C++ tests through the Python bindings.

Expected values (Java TournamentScheduleConstraintProviderTest scenarios):
  unfairness = round6(sqrt(sum(load^2) - sum(load)^2 / n)),
  soft = -(medium unfairness * 1e6 + soft unfairness).
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
    # Java test: T0 on D0 three times (3 pairs), T1 on D1/D2 (0 pairs) -> hard -3.
    # medium: loads {3, 2} -> 0.707107; no head-to-heads (same-team pairs are excluded).
    schedule = make_schedule([(0, 0), (0, 0), (0, 0), (1, 1), (2, 1)])
    score = calculate_score(schedule)
    assert score.hard == -3
    assert score.soft == pytest.approx(-707107.0, abs=1e-6)


def test_unavailability_penalty():
    # Java test: unavailable (T0,D0)/(T1,D1) both hit + T1 twice on D1 (1 pair).
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
    # Java test: once per team -> perfectly fair.
    schedule = make_schedule([(0, 0), (1, 1), (2, 2)])
    score = calculate_score(schedule)
    assert score.hard == 0
    assert score.soft == 0.0
    # Only T2 twice -> loads {1,1,2} = 0.816497 medium (one head-to-head pair, so soft 0).
    schedule = make_schedule([(0, 0), (1, 1), (2, 2), (0, 2)])
    score = calculate_score(schedule)
    assert score.hard == 0
    assert score.soft == pytest.approx(-816497.0, abs=1e-6)


def test_evenly_confrontation_count():
    # Java test: three teams meet each other once on D0 -> perfectly even.
    schedule = make_schedule([(0, 0), (0, 1), (0, 2)])
    score = calculate_score(schedule)
    assert score.hard == 0
    assert score.soft == 0.0
    # + T2 once more: head-to-heads (0,1)=1 (0,2)=2 (1,2)=2 -> 0.816497 soft,
    # loads {1,1,2} -> 0.816497 medium, T2's same-day pair -> hard -1.
    schedule = make_schedule([(0, 0), (0, 1), (0, 2), (0, 2)])
    score = calculate_score(schedule)
    assert score.hard == -1
    assert score.soft == pytest.approx(-816497.816497, abs=1e-6)


def test_unassigned_contributes_nothing():
    schedule = make_schedule([(0, -1), (0, 1)], penalties=[(0, 0)])
    score = calculate_score(schedule)
    assert score.hard == 0
    assert score.soft == 0.0
