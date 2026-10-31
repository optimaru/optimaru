"""Demo solve: the instance reproducing Java DemoDataGenerator reaches feasible
and pinned assignments are kept."""

import tournament_scheduling as ts
from tournament_scheduling.models import TournamentScheduleModel, demo_data


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
