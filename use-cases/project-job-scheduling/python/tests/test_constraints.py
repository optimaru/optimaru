"""Constraint spot checks: reproduces the hand-computed cases of Java
ProjectJobSchedulingConstraintProviderTest through the pydantic model -> engine conversion."""

import project_job as pj
from project_job.models import (
    AllocationModel,
    ExecutionModeModel,
    JobModel,
    ProjectJobScheduleModel,
    ProjectModel,
    ResourceModel,
    ResourceRequirementModel,
)


def _job(job_id, duration, requirement, successors=(), job_type="STANDARD"):
    reqs = [ResourceRequirementModel(resource_id="R", requirement=requirement)] if requirement else []
    return JobModel(
        id=job_id,
        project_id="P",
        job_type=job_type,
        execution_modes=[ExecutionModeModel(id=f"m{job_id}", duration=duration, resource_requirements=reqs)],
        successor_job_ids=list(successors),
    )


def _schedule(jobs, delays, renewable, critical_path_duration=0, capacity=10):
    """Single-resource/project schedule. delays[job_id] None means uninitialized."""
    allocations = [
        AllocationModel(
            id=j.id,
            job_id=j.id,
            execution_mode_id=None if delays.get(j.id) is None else j.execution_modes[0].id,
            delay=delays.get(j.id),
        )
        for j in jobs
    ]
    return ProjectJobScheduleModel(
        projects=[ProjectModel(id="P", release_date=0, critical_path_duration=critical_path_duration)],
        resources=[
            ResourceModel(id="R", capacity=capacity, renewable=renewable, project_id="P"),
        ],
        jobs=jobs,
        allocations=allocations,
    )


def test_non_renewable_resource_capacity():
    # Java: demand 7 + 6, capacity 10 (non-renewable, date-independent) -> hard -3.
    model = _schedule(
        [_job("1", 10, 7), _job("2", 10, 6)],
        {"1": 100, "2": 100},
        renewable=False,
    )
    score = pj.calculate_score(model.to_core())
    assert (score.hard, score.medium, score.soft) == (-3, 0, 0)


def test_renewable_resource_capacity():
    # Java: predecessorsDoneDate 1/2 (here predecessor jobs of duration 1/2), delay
    # 100, duration 10, demand 7/5, capacity 10 -> 9 overlapping days x 2 = hard -18.
    model = _schedule(
        [
            _job("p1", 1, 0, successors=["1"]),
            _job("p2", 2, 0, successors=["2"]),
            _job("1", 10, 7),
            _job("2", 10, 5),
        ],
        {"p1": 0, "p2": 0, "1": 100, "2": 100},
        renewable=True,
    )
    core = model.to_core()
    score = pj.calculate_score(core)
    assert (score.hard, score.medium, score.soft) == (-18, 0, 0)
    # The ported shadow chain: predecessorsDoneDate -> startDate -> endDate.
    solved = type(model).from_core(core)
    by_id = {a.id: a for a in solved.allocations}
    assert (by_id["1"].start_date, by_id["1"].end_date) == (101, 111)
    assert (by_id["2"].start_date, by_id["2"].end_date) == (102, 112)


def test_total_project_delay_and_makespan():
    # Java totalProjectDelay: SINK end 111/112, critical path end 100 ->
    # medium -(11 + 12) = -23; totalMakespan: soft -112; renewable overlap is
    # hard -18 as above.
    model = _schedule(
        [
            _job("p1", 1, 0, successors=["1"]),
            _job("p2", 2, 0, successors=["2"]),
            _job("1", 10, 7, job_type="SINK"),
            _job("2", 10, 5, job_type="SINK"),
        ],
        {"p1": 0, "p2": 0, "1": 100, "2": 100},
        renewable=True,
        critical_path_duration=100,
    )
    score = pj.calculate_score(model.to_core())
    assert (score.hard, score.medium, score.soft) == (-18, -23, -112)


def test_project_delay_not_positive():
    # Java totalMakespan case (criticalPathDuration 155): a negative delay gives
    # no medium penalty; the makespan stays.
    model = _schedule(
        [
            _job("p1", 1, 0, successors=["1"]),
            _job("p2", 2, 0, successors=["2"]),
            _job("1", 10, 7, job_type="SINK"),
            _job("2", 10, 5, job_type="SINK"),
        ],
        {"p1": 0, "p2": 0, "1": 100, "2": 100},
        renewable=True,
        critical_path_duration=155,
    )
    score = pj.calculate_score(model.to_core())
    assert (score.hard, score.medium, score.soft) == (-18, 0, -112)


def test_unassigned_contributes_nothing():
    # Uninitialized allocations use no resources and only pass dates through (Java's null
    # executionMode/delay semantics).
    model = _schedule(
        [_job("a", 2, 1, successors=["b"]), _job("b", 5, 9), _job("c", 3, 9)],
        {"a": 1, "b": None, "c": None},
        renewable=True,
        capacity=1,
    )
    core = model.to_core()
    score = pj.calculate_score(core)
    assert (score.hard, score.medium, score.soft) == (0, 0, 0)
    solved = type(model).from_core(core)
    by_id = {a.id: a for a in solved.allocations}
    assert (by_id["b"].start_date, by_id["b"].end_date) == (3, 3)
