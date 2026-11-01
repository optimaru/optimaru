"""Demo solve checks: the Java demo instance reaches feasible with a consistent
assignment, and pinned jobs and the unassigned pool are respected."""

import time

import pytest

import food_packaging as fp


def test_demo_data_matches_java_shape():
    schedule = fp.generate_demo_data()
    assert len(schedule.products) == 60  # 10 ingredients x 6
    assert schedule.line_count() == 5
    assert len(schedule.operators) == 5
    assert len(schedule.jobs) == 100
    assert schedule.to_minute - schedule.from_minute == 14 * 24 * 60

    names = [job.name for job in schedule.jobs]
    assert names == sorted(names)  # Java: jobs.sort(comparing(getName))
    for job in schedule.jobs:
        assert 120 <= job.duration_minutes < 300
        assert job.min_start_minute % (24 * 60) == 0
        assert job.ideal_end_minute % (24 * 60) == 16 * 60  # 16:00
        assert job.max_end_minute >= job.ideal_end_minute + 24 * 60

    count = len(schedule.products)
    for prev in range(count):
        assert schedule.cleanup_of(prev, prev) == 0
        for cur in range(count):
            if prev != cur:
                assert schedule.cleanup_of(prev, cur) == 10 or 30 <= schedule.cleanup_of(prev, cur) < 60

    small = fp.generate_demo_data(line_count=2, job_count=10)
    assert small.line_count() == 2
    assert len(small.jobs) == 10


def test_demo_solve_reaches_feasible():
    schedule = fp.generate_demo_data()
    steps = []
    fp.solve(
        schedule,
        time_limit_seconds=15.0,
        random_seed=42,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: steps.append((step, score.hard)),
    )

    assert schedule.score.is_feasible()
    assert schedule.score.hard == 0
    assert steps  # on_new_best fired

    # Assignment consistency: every job is in exactly one list, the shadow chain is
    # sequential, and the hard constraints really hold.
    membership = [0] * len(schedule.jobs)
    for line in schedule.lines:
        for j in line.jobs:
            membership[j] += 1
    for j in schedule.unassigned_jobs:
        membership[j] += 1
    assert all(m == 1 for m in membership)

    jobs = schedule.jobs
    for line_index, line in enumerate(schedule.lines):
        previous_end = line.start_minute
        for j in line.jobs:
            job = jobs[j]
            assert job.line_index == line_index
            assert job.start_cleaning_minute == previous_end
            assert job.start_production_minute >= job.start_cleaning_minute
            assert job.end_minute == job.start_production_minute + job.duration_minutes
            assert job.end_minute <= job.max_end_minute  # hard 0
            previous_end = job.end_minute

    # The demo has operators 5 : lines 5, so only an injective assignment guarantees 0 conflicts.
    operator_indices = [line.operator_index for line in schedule.lines]
    assert sorted(operator_indices) == [0, 1, 2, 3, 4]


def test_pinned_jobs_are_kept():
    schedule = fp.generate_demo_data(line_count=3, job_count=18)
    # Pin job 0 to the front of line 0 and pin job 1 as unassigned.
    jobs = schedule.jobs
    jobs[0].pinned = True
    jobs[1].pinned = True
    schedule.jobs = jobs
    lines = schedule.lines
    lines[0].jobs = [0]
    schedule.lines = lines

    fp.solve(schedule, time_limit_seconds=3.0, random_seed=7)

    assert schedule.lines[0].jobs[0] == 0
    assert schedule.jobs[0].line_index == 0
    assert schedule.jobs[1].line_index == -1
    assert 1 in schedule.unassigned_jobs
    # The pinned unassigned job's duration remains in the medium penalty.
    assert sum(fp.constraint_costs(schedule)["unassigned_jobs_cost"]["costs"]) >= schedule.jobs[1].duration_minutes


def test_solver_config_roundtrip():
    config = fp.SolverConfig()
    config.spent_limit_seconds = 1.5
    config.random_seed = 11
    config.parallel_solver_count = 2
    config.operator_rounds = 2
    config.best_score_limit = (0, float("-inf"))
    assert config.spent_limit_seconds == 1.5
    assert config.parallel_solver_count == 2
    assert config.operator_rounds == 2
    assert config.best_score_limit == (0, float("-inf"))

    schedule = fp.generate_demo_data(line_count=2, job_count=8)
    fp.solve(schedule, config=config)
    assert schedule.score == fp.calculate_score(schedule)


def _boom(_step, _score):
    raise RuntimeError("boom")


def test_a_raising_on_new_best_raises_from_a_multi_start_solve():
    """Two parallel starts: the callback's exception reaches Python instead of aborting the process,
    and the other start stops at once instead of running out its 10 s budget."""
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        fp.solve(
            fp.generate_demo_data(line_count=2, job_count=8),
            time_limit_seconds=10.0,
            parallel_solver_count=2,
            on_new_best=_boom,
        )
    assert time.monotonic() - started < 8.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path used to call std::terminate."""
    from food_packaging import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            fp.solve(fp.generate_demo_data(line_count=2, job_count=8), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)
