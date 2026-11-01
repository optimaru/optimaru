from itertools import product

import pytest

import sports_league as domain
from sports_league.constraints import (
    AwayToAwayHopCost,
    AwayToEndHopCost,
    AwayToHomeHopCost,
    ConsecutiveHomeMatchesCost,
    HomeToAwayHopCost,
    Level,
    RepeatMatchOnNextDayCost,
    StartToAwayHopCost,
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
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }


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


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------


def _three_rounds() -> LeagueScheduleModel:
    """Three teams, rounds 0-2 (1 a weekend), three matches (a derby, its return
    fixture, one more): 27 assignments."""
    return LeagueScheduleModel(
        teams=[TeamModel(id=str(team)) for team in range(3)],
        rounds=[
            RoundModel(index=0),
            RoundModel(index=1, weekend_or_holiday=True),
            RoundModel(index=2),
        ],
        matches=[
            MatchModel(id="a", home_team=0, away_team=1, classic_match=True),
            MatchModel(id="b", home_team=1, away_team=0, classic_match=True),
            MatchModel(id="c", home_team=2, away_team=0),
        ],
        distance_to_team=[[0, 11, 12], [21, 0, 22], [31, 32, 0]],
    )


SELECTIONS = {
    "repeat": [
        TeamConflictCost(),
        TeamConflictCost(weight=3, offset=1),
        HomeToAwayHopCost(),
        StartToAwayHopCost(),
    ],
    "repeat-literal": [
        StartToAwayHopCost(offset=10, clip=False),
        StartToAwayHopCost(weight=2, offset=-3),
        AwayToEndHopCost(),
        AwayToEndHopCost(weight=2, offset=20, clip=False),
        WorkdayClassicMatchesCost(offset=1, clip=False),
    ],
    "hard-offsets": [
        TeamConflictCost(offset=1, clip=False),
        RepeatMatchOnNextDayCost(weight=2, offset=-1),
        ConsecutiveHomeMatchesCost(offset=2, clip=False),
        AwayToAwayHopCost(level="hard", offset=10),
        AwayToHomeHopCost(offset=5, clip=False),
    ],
    "identical-twice": [
        HomeToAwayHopCost(),
        HomeToAwayHopCost(),
        TeamConflictCost(),
    ],
    # Each pick on its own level: a soft literal team conflict next to the hard default,
    # the start hop on hard beyond 10 next to its soft default, the derby moved to hard.
    "levels": [
        TeamConflictCost(),
        TeamConflictCost(level="soft", weight=3, offset=1, clip=False),
        StartToAwayHopCost(),
        StartToAwayHopCost(level=Level.HARD, offset=10),
        WorkdayClassicMatchesCost(level="hard", weight=2),
        HomeToAwayHopCost(level="hard", offset=-1),
    ],
}


def _pinned(problem, fixed):
    candidate = problem.model_copy(deep=True)
    for match, round_ in zip(candidate.matches, fixed, strict=True):
        match.round = round_
    return candidate.to_core()


def _engine_of(problem, fixed, selection):
    """The Python score() mirror of the pinned rounds under `selection`."""
    core = _pinned(problem, fixed)
    return tuple(score(domain.constraint_costs(core, selection), selection))


def _engine_solve(problem, fixed, selection):
    """The engine's own score of the pinned rounds: a solve with no time keeps them."""
    core = _pinned(problem, fixed)
    assignment = core.assignment()
    domain.solve(core, time_limit_seconds=0, random_seed=1, constraints=selection)
    assert core.assignment() == assignment
    return core.score.hard, core.score.soft


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_every_assignment(selection) -> None:
    """Pin each of the 27 assignments: the CP-SAT objective's own levels are the engine's
    score under the same repeated / shifted picks (its own and the score() mirror's),
    exactly (every round is a term of a unit constraint, an empty one included); the
    reported score re-prices the default costs (each name once) to the same value."""
    problem = _three_rounds()
    for fixed in product(range(3), repeat=3):
        proxy: dict[str, int] = {}
        solved = solve(
            problem,
            time_limit_seconds=5,
            constraints=selection,
            _fixed_rounds=list(fixed),
            _proxy_score=proxy,
        )
        engine = _engine_of(problem, fixed, selection)
        assert _engine_solve(problem, fixed, selection) == engine, fixed
        assert (proxy["hard"], proxy["soft"]) == engine, fixed
        assert (solved.score_hard, solved.score_soft) == engine, fixed


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_reach_the_brute_force_optimum(selection) -> None:
    problem = _three_rounds()
    best = max(
        _engine_of(problem, fixed, selection) for fixed in product(range(3), repeat=3)
    )
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == best


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, level, offset, clip} dicts pick the same instances as specs."""
    specs = [
        HomeToAwayHopCost(),
        HomeToAwayHopCost(weight=3, offset=10, clip=False),
        HomeToAwayHopCost(level="hard", offset=20),
    ]
    dicts = [
        {"name": "home_to_away_hop_cost"},
        {"name": "home_to_away_hop_cost", "weight": 3, "offset": 10, "clip": False},
        {"name": "home_to_away_hop_cost", "level": "hard", "offset": 20},
    ]
    problem = _three_rounds()
    assert solve(problem, time_limit_seconds=5, constraints=dicts) == solve(
        problem, time_limit_seconds=5, constraints=specs
    )


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(
            _three_rounds(),
            time_limit_seconds=1,
            constraints=[StartToAwayHopCost(offset=0.5)],
        )


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_like_the_engine(level) -> None:
    """The domain has no medium level: CP-SAT refuses the pick as the engine does, a repeat too."""
    message = "constraint 'home_to_away_hop_cost': this use case has no medium level"
    for selection in (
        [HomeToAwayHopCost(level=level)],
        [HomeToAwayHopCost(), HomeToAwayHopCost(level=level)],
    ):
        with pytest.raises(ValueError, match=message):
            solve(_three_rounds(), time_limit_seconds=1, constraints=selection)
        with pytest.raises(ValueError, match=message):
            domain.solve(
                _three_rounds().to_core(), time_limit_seconds=0, constraints=selection
            )
