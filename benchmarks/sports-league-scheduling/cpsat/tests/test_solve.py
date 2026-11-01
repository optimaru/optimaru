import pytest

import sports_league as domain
from sports_league.constraints import (
    TeamConflictCost,
    WorkdayClassicMatchesCost,
    get_constraints,
    score,
)
from sports_league.models import LeagueScheduleModel, MatchModel, RoundModel, TeamModel
from sports_league_cpsat import solve
from sports_league import calculate_score


def test_shared_team_matches_use_different_rounds() -> None:
    problem = LeagueScheduleModel(
        teams=[TeamModel(id="a"), TeamModel(id="b"), TeamModel(id="c")],
        rounds=[RoundModel(index=0), RoundModel(index=1)],
        matches=[
            MatchModel(id="ab", home_team=0, away_team=1),
            MatchModel(id="ac", home_team=0, away_team=2),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert solved.matches[0].round != solved.matches[1].round
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)


def _parity_problem() -> LeagueScheduleModel:
    return LeagueScheduleModel(
        teams=[TeamModel(id=str(team)) for team in range(4)],
        rounds=[
            RoundModel(index=0),
            RoundModel(index=1),
            RoundModel(index=2, weekend_or_holiday=True),
            RoundModel(index=3),
        ],
        matches=[
            MatchModel(id="a", home_team=0, away_team=1, classic_match=True),
            MatchModel(id="b", home_team=0, away_team=2),
            MatchModel(id="c", home_team=0, away_team=3),
            MatchModel(id="d", home_team=0, away_team=1),
            MatchModel(id="e", home_team=2, away_team=1),
            MatchModel(id="f", home_team=3, away_team=1),
            MatchModel(id="g", home_team=1, away_team=0),
        ],
        distance_to_team=[
            [0, 11, 12, 13],
            [21, 0, 22, 23],
            [31, 32, 0, 33],
            [41, 42, 43, 0],
        ],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_a_fixed_schedule(constraint) -> None:
    problem = _parity_problem()
    fixed_rounds = [0, 1, 2, 3, 1, 2, 1]
    candidate = problem.model_copy(deep=True)
    for match, round_ in zip(candidate.matches, fixed_rounds, strict=True):
        match.round = round_
    expected = score(
        domain.constraint_costs(candidate.to_core()),
        [type(constraint)(weight=1)],
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_rounds=fixed_rounds,
        _proxy_score=proxy,
    )

    assert (proxy["hard"], proxy["soft"]) == pytest.approx(expected)


@pytest.mark.parametrize(
    "name,fixed_rounds",
    [
        ("consecutive_home_matches_cost", [0, 1, 2]),
        ("consecutive_away_matches_cost", [0, 1, 2]),
    ],
)
def test_three_consecutive_matches_are_not_a_four_match_violation(
    name, fixed_rounds
) -> None:
    problem = LeagueScheduleModel(
        teams=[TeamModel(id="home"), TeamModel(id="away")],
        rounds=[RoundModel(index=index) for index in range(3)],
        matches=[
            MatchModel(id=str(index), home_team=0, away_team=1) for index in range(3)
        ],
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": name, "weight": 1}],
        _fixed_rounds=fixed_rounds,
        _proxy_score=proxy,
    )

    assert proxy["hard"] == 0


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info()."""
    from sports_league_cpsat import catalog

    engine = {row.name: row for row in domain.constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == domain.list_constraints()
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert entry["kind"] == "either"  # every sports-league constraint is flippable
        assert entry["params"] == {"weight": None, "level": None, "exponent": 1.0}


def test_workday_derby_default_weight_is_the_engines_1000() -> None:
    """Before 0.2.0 the CP-SAT derby plugin defaulted to weight 1 while the engine
    priced a workday derby at 1000; both now read 1000 from constraint_info()."""
    from sports_league_cpsat import catalog

    derby = next(
        entry for entry in catalog() if entry["name"] == "workday_classic_matches_cost"
    )
    assert derby["default_weight"] == 1000
    problem = _parity_problem()
    fixed_rounds = [0, 1, 2, 3, 1, 2, 1]  # the derby "a" sits on the workday round 0
    candidate = problem.model_copy(deep=True)
    for match, round_ in zip(candidate.matches, fixed_rounds, strict=True):
        match.round = round_
    expected = score(
        domain.constraint_costs(candidate.to_core()), [WorkdayClassicMatchesCost()]
    )
    assert expected == (0, -1000.0)
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[WorkdayClassicMatchesCost()],
        _fixed_rounds=fixed_rounds,
        _proxy_score=proxy,
    )

    assert (proxy["hard"], proxy["soft"]) == pytest.approx(expected)


def test_default_selection_scores_like_the_engine() -> None:
    """With no selection both solvers price the same solution identically."""
    problem = _parity_problem()
    fixed_rounds = [0, 1, 2, 3, 1, 2, 1]
    candidate = problem.model_copy(deep=True)
    for match, round_ in zip(candidate.matches, fixed_rounds, strict=True):
        match.round = round_
    engine = calculate_score(candidate.to_core())
    proxy = {}

    solved = solve(
        problem, time_limit_seconds=2, _fixed_rounds=fixed_rounds, _proxy_score=proxy
    )

    assert (proxy["hard"], proxy["soft"]) == pytest.approx((engine.hard, engine.soft))
    assert (solved.score_hard, solved.score_soft) == pytest.approx(
        (engine.hard, engine.soft)
    )


def test_demoting_team_conflict_scores_like_the_engine() -> None:
    """level="soft" lands on soft in both solvers: CP-SAT's reported score is the
    engine's score of the same solution under the same selection."""
    problem = _parity_problem()
    fixed_rounds = [0, 0, 2, 3, 1, 2, 1]  # "a" and "b" share team 0 on round 0
    selection = [TeamConflictCost(level="soft"), *get_constraints()[:3]]
    candidate = problem.model_copy(deep=True)
    for match, round_ in zip(candidate.matches, fixed_rounds, strict=True):
        match.round = round_
    engine = score(domain.constraint_costs(candidate.to_core()), selection)
    assert engine.hard == 0 and engine.soft < 0
    proxy = {}

    solved = solve(
        problem,
        time_limit_seconds=2,
        constraints=selection,
        _fixed_rounds=fixed_rounds,
        _proxy_score=proxy,
    )

    assert (proxy["hard"], proxy["soft"]) == pytest.approx(tuple(engine))
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
