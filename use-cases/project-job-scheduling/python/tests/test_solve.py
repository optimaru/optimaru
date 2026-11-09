"""Demo instance solve test: reaching feasible (hard 0) and result consistency."""

import project_job as pj
from project_job.demo import generate_demo_data
from project_job.models import ProjectJobScheduleModel


def test_demo_solve_reaches_feasible():
    model = generate_demo_data()
    result = pj.solve(model, time_limit_seconds=5.0, random_seed=42)

    assert isinstance(result, ProjectJobScheduleModel)
    assert result.score_hard == 0
    # Every allocation is initialized and the SOURCE/SINK fixed values are kept.
    for job, allocation in zip(result.jobs, result.allocations):
        assert allocation.execution_mode_id is not None
        assert allocation.delay is not None
        if job.job_type != "STANDARD":
            assert allocation.execution_mode_id == job.execution_modes[0].id
            assert allocation.delay == 0
    # The result score must match a rescoring.
    replan = result.to_core()
    score = pj.calculate_score(replan)
    assert (score.hard, score.medium, score.soft) == (
        result.score_hard,
        result.score_medium,
        result.score_soft,
    )


def test_solve_core_schedule_in_place():
    schedule = generate_demo_data().to_core()
    returned = pj.solve(schedule, time_limit_seconds=3.0, random_seed=7, stop_at_first_feasible=True)
    assert returned is schedule
    assert schedule.score.is_feasible()
    value_count = schedule.value_count()
    assert all(0 <= value < value_count for value in schedule.assignment())
    # Derived dates are filled in and respect precedence.
    allocations = schedule.allocations
    for allocation in allocations:
        assert allocation.end_date >= allocation.start_date >= 0
        for predecessor in allocation.predecessor_allocations:
            assert allocations[predecessor].end_date <= allocation.start_date


def test_on_new_best_callback_fires():
    schedule = generate_demo_data().to_core()
    events: list[tuple[int, int]] = []
    pj.solve(
        schedule,
        time_limit_seconds=1.0,
        random_seed=1,
        on_new_best=lambda step, score: events.append((step, score.hard)),
    )
    assert events
    assert events[-1][1] == schedule.score.hard


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    import pytest

    def boom(_step, _score):
        raise RuntimeError("boom")

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        pj.solve(
            generate_demo_data().to_core(),
            time_limit_seconds=30.0,
            parallel_solver_count=4,
            on_new_best=boom,
        )
    assert time.monotonic() - started < 10.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path would call std::terminate."""
    import time

    import pytest

    from project_job import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            pj.solve(generate_demo_data().to_core(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_stop_at_first_feasible_has_no_soft_floor():
    """-(2**63) (INT64_MIN) on the soft-side levels means "no floor": the engine compares
    hard only, so no soft score, however low, misses the stop (toEngineScoreLimit,
    tested in the C++ suite)."""
    cfg = pj._build_config(
        time_limit_seconds=None,
        random_seed=None,
        parallel_solver_count=None,
        stop_at_first_feasible=True,
        full_assert=False,
        on_new_best=None,
        config=None,
    )
    assert cfg.best_score_limit == (0, -(2**63), -(2**63))
