"""Serialization/deserialization checks of the pydantic mirror (models.py)."""

import pytest

import sports_league as sl
from sports_league.models import LeagueScheduleModel, SolveConfig


def small_schedule() -> "sl.LeagueSchedule":
    teams = [sl.Team("1", "Alpha"), sl.Team("2", "Beta"), sl.Team("3", "Gamma")]
    rounds = [sl.Round(0), sl.Round(1, weekend_or_holiday=True)]
    matches = [
        sl.Match("1-2", 0, 1, classic_match=True, round=0),
        sl.Match("2-1", 1, 0),
        sl.Match("1-3", 0, 2, round=1),
    ]
    distance = [[0, 5, 7], [5, 0, 2], [7, 2, 0]]
    return sl.LeagueSchedule(teams, rounds, matches, distance)


def test_roundtrip_json():
    schedule = small_schedule()
    sl.calculate_score(schedule)

    model = LeagueScheduleModel.from_core(schedule)
    clone = LeagueScheduleModel.model_validate_json(model.model_dump_json())
    assert clone == model

    core = clone.to_core()
    assert [t.id for t in core.teams] == ["1", "2", "3"]
    assert [r.index for r in core.rounds] == [0, 1]
    assert core.rounds[1].weekend_or_holiday is True
    assert core.assignment() == schedule.assignment()
    assert core.matches[0].classic_match is True
    assert core.distance_to_team == schedule.distance_to_team
    # The engine recomputes score — same assignment, same score.
    assert sl.calculate_score(core).hard == schedule.score.hard
    assert sl.calculate_score(core).soft == schedule.score.soft


def test_from_core_records_score():
    schedule = small_schedule()
    score = sl.calculate_score(schedule)
    model = LeagueScheduleModel.from_core(schedule)
    assert model.score_hard == score.hard
    assert model.score_soft == score.soft


def test_solve_config_to_core():
    cfg = SolveConfig(time_limit_seconds=3.0, random_seed=7, stop_at_first_feasible=True)
    core = cfg.to_core()
    assert core.spent_limit_seconds == pytest.approx(3.0)
    assert core.random_seed == 7
    assert core.best_score_limit == (0, -1e18)

    with pytest.raises(ValueError):
        SolveConfig(ruin_recreate_min_ruin_fraction=0.5, ruin_recreate_max_ruin_fraction=0.1)


def _dump_with_round(value):
    data = LeagueScheduleModel.from_core(small_schedule()).model_dump()
    data["matches"][0]["round"] = value
    return data


@pytest.mark.parametrize("value", [-2, 2, 5, 99999999])
def test_a_round_out_of_range_is_refused_before_the_engine(value):
    with pytest.raises(ValueError, match=rf"match '1-2': round {value} is out of range -1\.\.1"):
        LeagueScheduleModel.model_validate(_dump_with_round(value))


@pytest.mark.parametrize("value", [-1, 0, 1])
def test_every_round_in_range_is_accepted(value):
    assert LeagueScheduleModel.model_validate(_dump_with_round(value)).to_core().matches[0].round == value


def test_a_round_edited_after_validation_is_refused_by_to_core():
    model = LeagueScheduleModel.from_core(small_schedule())
    model.matches[0].round = 99999999
    with pytest.raises(ValueError, match="out of range"):
        model.to_core()
