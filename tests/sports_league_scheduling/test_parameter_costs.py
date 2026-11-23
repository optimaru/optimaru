"""A parameter that acts only through a cost needs that cost in the selection."""

import pytest

import optimaru.sports_league_scheduling as sl
from optimaru.sports_league_scheduling.constraints import WorkdayClassicMatchesCost, get_constraints
from optimaru.sports_league_scheduling.models import LeagueScheduleModel, parameter_uses

QUICK = {"time_limit_seconds": 0.05, "random_seed": 1}
HOPS = (
    "start_to_away_hop_cost",
    "away_to_end_hop_cost",
    "home_to_away_hop_cost",
    "away_to_away_hop_cost",
    "away_to_home_hop_cost",
)


def demo() -> LeagueScheduleModel:
    return LeagueScheduleModel.from_core(sl.generate_demo_data())


def without(*names):
    return [spec for spec in get_constraints() if spec.name not in names]


def test_the_default_selection_takes_every_parameter():
    problem = demo()
    assert {use.field for use in parameter_uses(problem)} == {"classic_match", "weekend_or_holiday", "distance_to_team"}
    sl.solve(problem, **QUICK)


def test_a_selection_without_the_classic_cost_is_refused():
    with pytest.raises(
        ValueError, match="sets classic_match, but the constraints leave out workday_classic_matches_cost"
    ):
        sl.solve(demo(), constraints=without("workday_classic_matches_cost"), **QUICK)


def test_distances_need_one_hop_cost():
    with pytest.raises(
        ValueError, match="sets distance_to_team, but the constraints leave out start_to_away_hop_cost or"
    ):
        sl.solve(demo(), constraints=without(*HOPS), **QUICK)
    sl.solve(demo(), constraints=without(*HOPS[1:]), **QUICK)  # one of them is enough


def test_a_weight_zero_pick_counts():
    picks = [*without("workday_classic_matches_cost"), WorkdayClassicMatchesCost(weight=0)]
    sl.solve(demo(), constraints=picks, **QUICK)


def test_a_missing_cost_is_fine_when_its_parameter_is_unset():
    problem = demo()
    problem = problem.model_copy(
        update={
            "matches": [m.model_copy(update={"classic_match": False}) for m in problem.matches],
            "rounds": [r.model_copy(update={"weekend_or_holiday": False}) for r in problem.rounds],
        }
    )
    sl.solve(problem, constraints=without("workday_classic_matches_cost"), **QUICK)
