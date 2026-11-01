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


def test_a_zero_soft_score_is_positive_zero():
    """The unsolved demo has no soft penalty: it scores 0soft, not -0soft (IEEE negative zero)."""
    import math

    import sports_league as _pkg

    score = _pkg.calculate_score(_pkg.demo_schedule())
    assert score.soft == 0.0
    assert math.copysign(1.0, score.soft) == 1.0
    assert "-0soft" not in repr(score)


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    def boom(_step, _score):
        raise RuntimeError("boom")

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        sl.solve(
            sl.demo_schedule(),
            time_limit_seconds=30.0,
            parallel_solver_count=4,
            on_new_best=boom,
        )
    assert time.monotonic() - started < 10.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path used to call std::terminate."""
    import time

    from sports_league import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            sl.solve(sl.demo_schedule(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_stop_at_first_feasible_stops_whatever_the_soft_total():
    """stop_at_first_feasible compares the hard level only (best_score_limit (0, -inf)).

    Every enabled soft penalty at weight 1e21 puts the soft score far below -1e18 at the
    first feasible solution: the solve still stops there instead of running its whole
    30 s. The first 0.2.0 limit, (0, -1e18), was never reached by such a score.
    """
    import dataclasses
    import time

    import sports_league as package
    from sports_league.constraints import get_constraints

    info = {row.name: row for row in package.constraint_info()}

    def heavy(spec):
        row = info[spec.name]
        soft_penalty = row.default_level == "soft" and row.default_enabled and not row.reward
        return dataclasses.replace(spec, weight=1e21) if soft_penalty else spec

    seen = []
    started = time.monotonic()
    package.solve(
        sl.demo_schedule(),
        time_limit_seconds=30.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda _step, score: seen.append(score),
        constraints=[heavy(spec) for spec in get_constraints()],
    )
    assert time.monotonic() - started < 20.0
    assert seen[-1].hard == 0 and seen[-1].soft < -1e18
