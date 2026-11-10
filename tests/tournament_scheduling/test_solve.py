"""Demo solve: the demo instance reaches feasible
and pinned assignments are kept."""

import optimaru.tournament_scheduling as ts
from optimaru.tournament_scheduling.models import TournamentScheduleModel, demo_data


def test_demo_solve_reaches_feasible():
    model = demo_data()
    result = ts.solve(model, time_limit_seconds=5.0, random_seed=42)
    assert isinstance(result, TournamentScheduleModel)
    assert result.score_hard == 0  # feasible
    assert all(a.team is not None for a in result.team_assignments)
    # Per-team assignment counts form a valid distribution (sum == 72; one match per day is hard 0).
    loads = [0] * len(result.teams)
    for a in result.team_assignments:
        loads[a.team] += 1
    assert sum(loads) == 72
    # Cannot beat the unfairness of the medium-optimal distribution (11,11,10,10,10,10,10).
    assert result.score_soft <= 0
    assert result.score_soft >= -(ts.unfairness([11, 11, 10, 10, 10, 10, 10]) * ts.MEDIUM_SCALE + 100)


def test_solve_core_in_place():
    core = demo_data().to_core()
    result = ts.solve(core, time_limit_seconds=2.0, random_seed=1)
    assert result is core
    assert core.score.is_feasible()
    assert all(t >= 0 for t in core.assignment())


def test_pinned_assignment_kept():
    model = demo_data()
    model.team_assignments[0].team = 6  # pin Maciej to Day 0 slot 0
    model.team_assignments[0].pinned = True
    result = ts.solve(model, time_limit_seconds=2.0, random_seed=7)
    assert result.team_assignments[0].team == 6
    assert result.team_assignments[0].pinned is True
    assert result.score_hard == 0


def test_stop_at_first_feasible_and_callback():
    events = []
    result = ts.solve(
        demo_data(),
        time_limit_seconds=5.0,
        random_seed=3,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: events.append((step, score.hard)),
    )
    assert result.score_hard == 0
    assert events, "the on_new_best callback was never called"


def test_parallel_solver_count_workers():
    """parallelSolverCount>1 fans out to domain worker threads rather than the engine's
    built-in multiStart (TeamCountMove must bind to each worker's calculator).
    Checks that each worker's independent solve ends normally and the best is adopted."""
    for workers in (2, 4):
        core = demo_data().to_core()
        ts.solve(core, time_limit_seconds=2.0, random_seed=42, parallel_solver_count=workers)
        assert core.score.is_feasible()
        assert all(t >= 0 for t in core.assignment())


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    import pytest

    def boom(_step, _score):
        raise RuntimeError("boom")

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        ts.solve(demo_data(), time_limit_seconds=30.0, parallel_solver_count=4, on_new_best=boom)
    assert time.monotonic() - started < 10.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path would call std::terminate."""
    import time

    import pytest

    from optimaru.tournament_scheduling import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            ts.solve(demo_data(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_stop_at_first_feasible_stops_whatever_the_soft_total():
    """stop_at_first_feasible compares the hard level only (best_score_limit (0, -inf)).

    Every enabled soft penalty at weight 1e21 puts the soft score far below -1e18 at the
    first feasible solution: the solve still stops there instead of running its whole
    30 s. A fixed limit such as (0, -1e18) would never be reached by such a score.
    """
    import dataclasses
    import time

    import optimaru.tournament_scheduling as package
    from optimaru.tournament_scheduling.constraints import get_constraints

    info = {row.name: row for row in package.constraint_info()}

    def heavy(spec):
        row = info[spec.name]
        soft_penalty = row.default_level == "soft" and row.default_enabled and not row.reward
        return dataclasses.replace(spec, weight=1e21) if soft_penalty else spec

    seen = []
    started = time.monotonic()
    package.solve(
        demo_data(),
        time_limit_seconds=30.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda _step, score: seen.append(score),
        constraints=[heavy(spec) for spec in get_constraints()],
    )
    assert time.monotonic() - started < 20.0
    assert seen[-1].hard == 0 and seen[-1].soft < -1e18
