from itertools import product

import pytest

import tournament_scheduling as domain
from tournament_scheduling.constraints import get_constraints, score
from tournament_scheduling.models import (
    DayModel,
    TeamAssignmentModel,
    TeamModel,
    TournamentScheduleModel,
    UnavailabilityPenaltyModel,
)
from tournament_scheduling_cpsat import solve
from tournament_scheduling import calculate_score


def test_team_loads_are_balanced_without_day_conflicts() -> None:
    problem = TournamentScheduleModel(
        teams=[TeamModel(name="a"), TeamModel(name="b")],
        days=[DayModel(date_index=0), DayModel(date_index=1)],
        team_assignments=[
            TeamAssignmentModel(id=0, day=0),
            TeamAssignmentModel(id=1, day=1),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert {assignment.team for assignment in solved.team_assignments} == {0, 1}
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)


def _parity_problem() -> TournamentScheduleModel:
    return TournamentScheduleModel(
        teams=[TeamModel(name="a"), TeamModel(name="b"), TeamModel(name="c")],
        days=[DayModel(date_index=0), DayModel(date_index=1)],
        unavailability_penalties=[UnavailabilityPenaltyModel(team=0, day=0)],
        team_assignments=[
            TeamAssignmentModel(id=0, day=0, index_in_day=0),
            TeamAssignmentModel(id=1, day=0, index_in_day=1),
            TeamAssignmentModel(id=2, day=1, index_in_day=0),
            TeamAssignmentModel(id=3, day=1, index_in_day=1),
        ],
    )


def _brute_force_score(problem, constraint):
    best = None
    for teams in product(
        range(len(problem.teams)), repeat=len(problem.team_assignments)
    ):
        candidate = problem.model_copy(deep=True)
        for assignment, team in zip(candidate.team_assignments, teams, strict=True):
            assignment.team = team
        value = score(domain.constraint_costs(candidate.to_core()), [constraint])
        best = max(best, value) if best is not None else value
    return best


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_brute_force_independently(constraint) -> None:
    problem = _parity_problem()
    expected = _brute_force_score(problem, constraint)

    solved = solve(problem, time_limit_seconds=2, constraints=[constraint])

    assert (solved.score_hard, solved.score_soft) == pytest.approx(expected)
