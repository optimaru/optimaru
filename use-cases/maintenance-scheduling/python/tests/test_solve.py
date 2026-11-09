"""Demo solve checks: the SMALL demo reaches feasible with a consistent assignment."""

from itertools import pairwise

import maintenance_scheduling as mnt

# The demo instance on a fixed Monday (2021-02-01).
MONDAY = 18659


def test_demo_data_shape():
    small = mnt.generate_demo_data("SMALL", MONDAY)
    assert small.crew_count() == 3
    assert len(small.jobs) == 14
    assert small.to_date - small.from_date == 8 * 7
    assert small.date_count() == 8 * 5  # weekday-only value range

    large = mnt.generate_demo_data("LARGE", MONDAY)
    assert large.crew_count() == 5
    assert len(large.jobs) == 48

    # The min/max/ideal windows of the demo jobs must be consistent.
    for i, job in enumerate(small.jobs):
        assert 1 <= job.duration_in_days <= 10
        assert job.min_start_date is not None
        assert job.ideal_end_date is not None
        assert job.max_end_date is not None
        assert job.min_start_date < job.ideal_end_date <= job.max_end_date
        assert mnt.plus_business_days(job.min_start_date, job.duration_in_days) <= job.max_end_date, i


def test_demo_solve_reaches_feasible():
    schedule = mnt.generate_demo_data("SMALL", MONDAY)
    steps = []
    mnt.solve(
        schedule,
        time_limit_seconds=15.0,
        random_seed=42,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: steps.append((step, score.hard)),
    )

    assert schedule.score.is_feasible()
    assert schedule.score.hard == 0
    assert steps  # on_new_best fired

    # Every job assigned + values within the value range.
    assignment = schedule.assignment()
    assert all(0 <= v < schedule.crew_date_count() for v in assignment)

    # Check hard 0: [start, end) never overlaps per crew and stays within the min/max window.
    by_crew: dict[int, list[tuple[int, int]]] = {}
    for i, job in enumerate(schedule.jobs):
        start = schedule.job_start_date(i)
        end = schedule.job_end_date(i)
        assert start is not None and end is not None
        assert end == mnt.plus_business_days(start, job.duration_in_days)
        assert job.min_start_date <= start
        assert end <= job.max_end_date
        by_crew.setdefault(schedule.crew_index_of(job.crew_date), []).append((start, end))
    for intervals in by_crew.values():
        intervals.sort()
        for (s1, e1), (s2, _) in pairwise(intervals):
            assert e1 <= s2, (s1, e1, s2)


def test_pinned_job_is_kept():
    schedule = mnt.generate_demo_data("SMALL", MONDAY)
    # Pin job 0 to the first start date in its window on the Gamma crew.
    job0 = schedule.jobs[0]
    starts = schedule.start_dates()
    date_index = next(
        i
        for i, epoch in enumerate(starts)
        if epoch >= job0.min_start_date and mnt.plus_business_days(epoch, job0.duration_in_days) <= job0.max_end_date
    )
    pinned_value = schedule.crew_date_of(date_index, 2)
    jobs = schedule.jobs
    jobs[0].crew_date = pinned_value
    jobs[0].pinned = True
    schedule.jobs = jobs

    mnt.solve(schedule, time_limit_seconds=5.0, random_seed=7, stop_at_first_feasible=True)

    assert schedule.jobs[0].crew_date == pinned_value
    assert schedule.score.hard == 0


def test_solver_config_roundtrip():
    config = mnt.SolverConfig()
    config.spent_limit_seconds = 1.5
    config.random_seed = 11
    config.parallel_solver_count = 2
    config.best_score_limit = (0, float("-inf"))
    assert config.spent_limit_seconds == 1.5
    assert config.best_score_limit == (0, float("-inf"))

    schedule = mnt.generate_demo_data("SMALL", MONDAY)
    mnt.solve(schedule, config=config)
    assert schedule.score.hard is not None


def test_a_zero_soft_score_is_positive_zero():
    """The unsolved demo has no soft penalty: it scores 0soft, not -0soft (IEEE negative zero)."""
    import math

    import maintenance_scheduling as _pkg

    score = _pkg.calculate_score(_pkg.generate_demo_data())
    assert score.soft == 0.0
    assert math.copysign(1.0, score.soft) == 1.0
    assert "-0soft" not in repr(score)


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    import pytest

    def boom(_step, _score):
        raise RuntimeError("boom")

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        mnt.solve(
            mnt.generate_demo_data("SMALL", MONDAY),
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

    import pytest

    from maintenance_scheduling import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            mnt.solve(mnt.generate_demo_data("SMALL", MONDAY), time_limit_seconds=30.0, parallel_solver_count=4)
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

    import maintenance_scheduling as package
    from maintenance_scheduling.constraints import get_constraints

    info = {row.name: row for row in package.constraint_info()}

    def heavy(spec):
        row = info[spec.name]
        soft_penalty = row.default_level == "soft" and row.default_enabled and not row.reward
        return dataclasses.replace(spec, weight=1e21) if soft_penalty else spec

    seen = []
    started = time.monotonic()
    package.solve(
        mnt.generate_demo_data("SMALL", MONDAY),
        time_limit_seconds=30.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda _step, score: seen.append(score),
        constraints=[heavy(spec) for spec in get_constraints()],
    )
    assert time.monotonic() - started < 20.0
    assert seen[-1].hard == 0 and seen[-1].soft < -1e18
