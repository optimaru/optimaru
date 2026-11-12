"""Demo instance solve test: reaching feasible (hard 0) and result consistency."""

import optimaru.project_job_scheduling as pj
from optimaru.project_job_scheduling.demo import generate_demo_data
from optimaru.project_job_scheduling.models import ProjectJobScheduleModel


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

    from optimaru.project_job_scheduling import _core

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


def _competing_activities(count: int = 30) -> ProjectJobScheduleModel:
    """One project of `count` activities in three layers that compete for two resources of capacity 6.

    Every activity lasts 2 to 5 days and takes 1 to 4 units of the one resource and 1 to 3 of the
    other, so two or three run side by side at most: the work on the resources is several times
    what the longest chain takes, and how the activities are packed decides the end.
    Made without a random generator, so the instance is the same on every platform.
    """
    from optimaru.project_job_scheduling.models import (
        AllocationModel,
        ExecutionModeModel,
        JobModel,
        ProjectModel,
        ResourceModel,
        ResourceRequirementModel,
    )

    layer = count // 3
    jobs = [
        JobModel(
            id="source",
            project_id="p",
            job_type="SOURCE",
            execution_modes=[ExecutionModeModel(id="source-mode", duration=0, resource_requirements=[])],
            successor_job_ids=[f"a{i}" for i in range(layer)],
        )
    ]
    for i in range(count):
        if i + layer < count:
            # Two activities of the next layer: the one below, and its neighbour.
            successors = sorted({f"a{i + layer}", f"a{(i + 1) % layer + (i // layer + 1) * layer}"})
        else:
            successors = ["sink"]
        requirements = [
            ResourceRequirementModel(resource_id="r0", requirement=1 + (i * 3) % 4),
            ResourceRequirementModel(resource_id="r1", requirement=1 + (i * 5) % 3),
        ]
        jobs.append(
            JobModel(
                id=f"a{i}",
                project_id="p",
                job_type="STANDARD",
                execution_modes=[
                    ExecutionModeModel(id=f"a{i}-mode", duration=2 + (i * 7) % 4, resource_requirements=requirements)
                ],
                successor_job_ids=successors,
            )
        )
    jobs.append(
        JobModel(
            id="sink",
            project_id="p",
            job_type="SINK",
            execution_modes=[ExecutionModeModel(id="sink-mode", duration=0, resource_requirements=[])],
            successor_job_ids=[],
        )
    )
    allocations = [
        AllocationModel(
            id=f"allocation-{job.id}",
            job_id=job.id,
            execution_mode_id=job.execution_modes[0].id if job.job_type != "STANDARD" else None,
            delay=0 if job.job_type != "STANDARD" else None,
            pinned=job.job_type != "STANDARD",
        )
        for job in jobs
    ]
    return ProjectJobScheduleModel(
        projects=[ProjectModel(id="p", release_date=0, critical_path_duration=15)],
        resources=[ResourceModel(id="r0", capacity=6), ResourceModel(id="r1", capacity=6)],
        jobs=jobs,
        allocations=allocations,
        delay_range_size=200,
    )


def _solved(model: ProjectJobScheduleModel, *, serial_schedule_moves: bool) -> ProjectJobScheduleModel:
    config = pj.SolverConfig()
    config.step_count_limit = 20_000
    config.serial_schedule_moves = serial_schedule_moves
    return pj.solve(model, config=config, random_seed=3, parallel_solver_count=1)


def test_serial_schedule_moves_end_the_project_earlier():
    config = pj.SolverConfig()
    assert config.serial_schedule_moves is True
    assert config.custom_move_probability == 0.2

    model = _competing_activities()
    with_moves = _solved(model, serial_schedule_moves=True)
    without = _solved(model, serial_schedule_moves=False)

    assert with_moves.score_hard == 0 and without.score_hard == 0
    # The makespan is the soft level, the project's delay past its critical path the medium one.
    assert with_moves.score_soft > without.score_soft
    assert with_moves.score_medium > without.score_medium


def test_serial_schedule_moves_can_be_switched_off():
    config = pj.SolverConfig()
    config.serial_schedule_moves = False
    config.step_count_limit = 20_000
    result = pj.solve(generate_demo_data(), config=config, random_seed=1)
    assert result.score_hard == 0
