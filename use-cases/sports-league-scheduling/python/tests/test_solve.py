"""High-level solve() checks: the demo instance reaches feasible (hard 0)."""

import pytest

import sports_league as sl
from sports_league.models import LeagueScheduleModel, SolveConfig


def test_demo_solve_feasible():
    schedule = sl.demo_schedule()
    result = sl.solve(
        schedule,
        time_limit_seconds=20.0,
        random_seed=42,
        stop_at_first_feasible=True,
    )
    assert result is schedule  # in place
    assert schedule.score.is_feasible()
    assert schedule.score.hard == 0
    assert all(m.round >= 0 for m in schedule.matches)

    # Re-check hard 1: no team plays two matches in the same round.
    seen: set[tuple[int, int]] = set()
    for m in schedule.matches:
        ri = schedule.rounds[m.round].index
        for team in (m.home_team, m.away_team):
            assert (team, ri) not in seen
            seen.add((team, ri))

    # The solved result also round-trips through the pydantic mirror.
    model = LeagueScheduleModel.from_core(schedule)
    clone = LeagueScheduleModel.model_validate_json(model.model_dump_json())
    assert clone.to_core().assignment() == schedule.assignment()


def test_solve_accepts_config_objects():
    schedule = sl.demo_schedule()
    sl.solve(
        schedule,
        config=SolveConfig(time_limit_seconds=2.0, random_seed=1),
    )
    assert all(m.round >= 0 for m in schedule.matches)

    with pytest.raises(ValueError, match="config"):
        sl.solve(sl.demo_schedule(), config=object())


def test_on_new_best_callback():
    schedule = sl.demo_schedule()
    bests: list[tuple[int, float]] = []
    sl.solve(
        schedule,
        time_limit_seconds=2.0,
        random_seed=0,
        on_new_best=lambda step, score: bests.append((score.hard, score.soft)),
    )
    assert bests, "on_new_best was never called"
    # Best scores improve monotonically (lexicographic, hard first).
    assert bests == sorted(bests)


def test_parallel_solver_count_workers():
    """parallelSolverCount>1 fans out to single-start workers rather than the engine's
    built-in multiStart (the guidance hooks bind to each worker's working solution/calculator).
    Checks that each worker ends normally and the best is adopted."""
    for workers in (2, 4):
        schedule = sl.demo_schedule()
        sl.solve(
            schedule,
            time_limit_seconds=2.0,
            random_seed=42,
            parallel_solver_count=workers,
            stop_at_first_feasible=True,
        )
        assert schedule.score.hard == 0
        assert all(m.round >= 0 for m in schedule.matches)
