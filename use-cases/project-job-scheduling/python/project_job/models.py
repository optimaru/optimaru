"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Project/Resource/Job(ExecutionMode)/Allocation/ProjectJobSchedule
1:1, but like the REST shape of the Java quickstart, references are id strings
(`project_id`/`resource_id`/`job_id`/`execution_mode_id`), and like Java's
Job.successorJobs, precedence lives in each job's `successor_job_ids`. The engine
conversion (`to_core`) turns ids into indices, successor lists into the allocations'
predecessor lists, and (execution_mode_id, delay) into the composite planning
value::

    from project_job.models import ProjectJobScheduleModel

    model = ProjectJobScheduleModel.model_validate_json(payload)
    schedule = model.to_core()                          # pydantic -> engine
    ...solve...
    solved = ProjectJobScheduleModel.from_core(schedule)  # engine -> pydantic

Allocations must be 1:1 with jobs (the Java DemoDataGenerator convention).
`allocations[i].execution_mode_id` None means uninitialized (delay must be None
too); the solver decides (mode, delay). SOURCE/SINK allocations are always
pinned, and when passed uninitialized the solver fixes them to (first mode, delay 0).
`pydantic` is an optional dependency (`pip install project_job[models]`).
"""

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core

__all__ = [
    "JOB_TYPES",
    "AllocationModel",
    "ExecutionModeModel",
    "JobModel",
    "ProjectJobScheduleModel",
    "ProjectModel",
    "ResourceModel",
    "ResourceRequirementModel",
]

JOB_TYPES = ("SOURCE", "STANDARD", "SINK")


class _Model(BaseModel):
    model_config = ConfigDict(extra="forbid")


class ProjectModel(_Model):
    """A project. release_date is only the reference of the 'Total project delay'
    constraint (critical_path_end = release_date + critical_path_duration) and, as in
    the Java quickstart, does not force the start."""

    id: str
    release_date: int = 0
    critical_path_duration: int = Field(default=0, ge=0)


class ResourceModel(_Model):
    """A resource. Renewable capacity is 'per day', otherwise 'summed over the whole horizon'.
    project_id None == global resource (Java's GlobalResource; always renewable)."""

    id: str
    capacity: int = Field(ge=0)
    renewable: bool = True
    project_id: str | None = None

    @model_validator(mode="after")
    def _global_is_renewable(self):
        if self.project_id is None and not self.renewable:
            raise ValueError("a global resource (project_id None) must be renewable (Java GlobalResource)")
        return self


class ResourceRequirementModel(_Model):
    """How much of one resource one execution mode uses."""

    resource_id: str
    requirement: int = Field(ge=0)


class ExecutionModeModel(_Model):
    """One execution mode of a job (duration + resource demand)."""

    id: str
    duration: int = Field(default=0, ge=0)
    resource_requirements: list[ResourceRequirementModel] = Field(default_factory=list)


class JobModel(_Model):
    """A job. SOURCE/SINK are the project's start/end milestones, by convention with one
    execution mode (duration 0) (Java quickstart). `successor_job_ids` defines
    precedence (successor jobs can start once this job ends)."""

    id: str
    project_id: str
    job_type: str = "STANDARD"
    execution_modes: list[ExecutionModeModel] = Field(default_factory=list)
    successor_job_ids: list[str] = Field(default_factory=list)

    @model_validator(mode="after")
    def _valid(self):
        if self.job_type not in JOB_TYPES:
            raise ValueError(f"job_type must be one of {JOB_TYPES}: {self.job_type!r}")
        if not self.execution_modes:
            raise ValueError(f"job {self.id!r} needs at least one execution mode (SOURCE/SINK use a zero-duration mode)")
        mode_ids = {m.id for m in self.execution_modes}
        if len(mode_ids) != len(self.execution_modes):
            raise ValueError(f"job {self.id!r} has duplicate execution mode ids")
        return self


class AllocationModel(_Model):
    """Planning entity: the allocation of one job (1:1 with jobs).
    (execution_mode_id, delay) is the planning variable pair (both None ==
    uninitialized). start_date/end_date are read-only results (Java shadow variables)."""

    id: str
    job_id: str
    execution_mode_id: str | None = None
    delay: int | None = Field(default=None, ge=0)
    pinned: bool = False
    start_date: int | None = None
    end_date: int | None = None

    @model_validator(mode="after")
    def _valid(self):
        if self.execution_mode_id is None and self.delay is not None:
            raise ValueError(f"allocation {self.id!r}: delay cannot be set without execution_mode_id")
        return self


class ProjectJobScheduleModel(_Model):
    """Planning solution mirror. score_* are read-only results, kept as separate fields."""

    projects: list[ProjectModel]
    resources: list[ResourceModel]
    jobs: list[JobModel]
    allocations: list[AllocationModel]
    delay_range_size: int = Field(default=500, ge=1)
    score_hard: int | None = None
    score_medium: int | None = None
    score_soft: int | None = None

    @model_validator(mode="after")
    def _valid_references(self):
        project_ids = {p.id for p in self.projects}
        resource_ids = {r.id for r in self.resources}
        job_ids = {j.id for j in self.jobs}
        if len(project_ids) != len(self.projects):
            raise ValueError("duplicate project id")
        if len(resource_ids) != len(self.resources):
            raise ValueError("duplicate resource id")
        if len(job_ids) != len(self.jobs):
            raise ValueError("duplicate job id")
        if len({a.id for a in self.allocations}) != len(self.allocations):
            raise ValueError("duplicate allocation id")
        for resource in self.resources:
            if resource.project_id is not None and resource.project_id not in project_ids:
                raise ValueError(f"resource {resource.id!r} has unknown project_id {resource.project_id!r}")
        for job in self.jobs:
            if job.project_id not in project_ids:
                raise ValueError(f"job {job.id!r} has unknown project_id {job.project_id!r}")
            for successor in job.successor_job_ids:
                if successor not in job_ids:
                    raise ValueError(f"job {job.id!r} has unknown successor {successor!r}")
            for mode in job.execution_modes:
                for req in mode.resource_requirements:
                    if req.resource_id not in resource_ids:
                        raise ValueError(f"mode {mode.id!r} has unknown resource_id {req.resource_id!r}")
        # Allocations are 1:1 with jobs (the Java DemoDataGenerator convention).
        allocated_jobs = [a.job_id for a in self.allocations]
        if sorted(allocated_jobs) != sorted(job_ids):
            raise ValueError("allocations must be exactly 1:1 with jobs")
        for allocation in self.allocations:
            job = next(j for j in self.jobs if j.id == allocation.job_id)
            if allocation.execution_mode_id is not None:
                mode_ids = {m.id for m in job.execution_modes}
                if allocation.execution_mode_id not in mode_ids:
                    raise ValueError(
                        f"allocation {allocation.id!r} execution_mode_id {allocation.execution_mode_id!r} is "
                        f"not a mode of job {job.id!r}"
                    )
            if allocation.delay is not None and allocation.delay >= self.delay_range_size:
                raise ValueError(f"allocation {allocation.id!r} delay must be below delay_range_size")
        return self

    def to_core(self) -> "_core.ProjectJobSchedule":
        """pydantic -> engine object: id references to indices, job successor lists to
        allocation predecessor lists, (execution_mode_id, delay) to the composite planning
        value (ValueError if the predecessor graph has a cycle). score/start/end fields
        are ignored since the engine recomputes them."""
        project_index = {p.id: i for i, p in enumerate(self.projects)}
        resource_index = {r.id: i for i, r in enumerate(self.resources)}
        job_index = {j.id: i for i, j in enumerate(self.jobs)}
        allocation_of_job = {a.job_id: i for i, a in enumerate(self.allocations)}

        predecessors: list[list[int]] = [[] for _ in self.allocations]
        for job in self.jobs:
            for successor in job.successor_job_ids:
                predecessors[allocation_of_job[successor]].append(allocation_of_job[job.id])

        schedule = _core.ProjectJobSchedule(
            [_core.Project(p.id, p.release_date, p.critical_path_duration) for p in self.projects],
            [
                _core.Resource(
                    r.id,
                    r.capacity,
                    r.renewable,
                    -1 if r.project_id is None else project_index[r.project_id],
                )
                for r in self.resources
            ],
            [
                _core.Job(
                    j.id,
                    project_index[j.project_id],
                    getattr(_core.JobType, j.job_type),
                    [
                        _core.ExecutionMode(
                            m.id,
                            m.duration,
                            [
                                _core.ResourceRequirement(resource_index[req.resource_id], req.requirement)
                                for req in m.resource_requirements
                            ],
                        )
                        for m in j.execution_modes
                    ],
                )
                for j in self.jobs
            ],
            [
                _core.Allocation(
                    a.id,
                    job_index[a.job_id],
                    predecessors[i],
                    -1,  # the composite value is filled below with encode_value
                    a.pinned,
                )
                for i, a in enumerate(self.allocations)
            ],
            self.delay_range_size,
        )
        assignment = []
        for allocation in self.allocations:
            if allocation.execution_mode_id is None:
                assignment.append(-1)
                continue
            job = next(j for j in self.jobs if j.id == allocation.job_id)
            mode_index = next(i for i, m in enumerate(job.execution_modes) if m.id == allocation.execution_mode_id)
            assignment.append(schedule.encode_value(allocation.delay or 0, mode_index))
        schedule.set_assignment(assignment)
        return schedule

    @classmethod
    def from_core(cls, schedule: "_core.ProjectJobSchedule") -> "ProjectJobScheduleModel":
        """engine object -> pydantic (including the current assignment, derived dates and
        score). Uninitialized (-1) allocations get execution_mode_id/delay None. Each job's
        successor_job_ids are filled by inverting the allocation predecessor lists."""
        successor_ids: dict[int, list[str]] = {i: [] for i in range(len(schedule.jobs))}
        allocations = schedule.allocations
        for allocation in allocations:
            for predecessor in allocation.predecessor_allocations:
                successor_ids[allocations[predecessor].job].append(schedule.jobs[allocation.job].id)

        allocation_models = []
        for a, allocation in enumerate(allocations):
            job = schedule.jobs[allocation.job]
            if allocation.mode_delay < 0:
                mode_id, delay = None, None
            else:
                mode_id = job.execution_modes[schedule.execution_mode_index_of_value(a, allocation.mode_delay)].id
                delay = schedule.delay_of_value(allocation.mode_delay)
            allocation_models.append(
                AllocationModel(
                    id=allocation.id,
                    job_id=job.id,
                    execution_mode_id=mode_id,
                    delay=delay,
                    pinned=allocation.pinned,
                    start_date=allocation.start_date,
                    end_date=allocation.end_date,
                )
            )

        return cls(
            projects=[
                ProjectModel(
                    id=p.id,
                    release_date=p.release_date,
                    critical_path_duration=p.critical_path_duration,
                )
                for p in schedule.projects
            ],
            resources=[
                ResourceModel(
                    id=r.id,
                    capacity=r.capacity,
                    renewable=r.renewable,
                    project_id=None if r.project < 0 else schedule.projects[r.project].id,
                )
                for r in schedule.resources
            ],
            jobs=[
                JobModel(
                    id=j.id,
                    project_id=schedule.projects[j.project].id,
                    job_type=j.job_type.name,
                    execution_modes=[
                        ExecutionModeModel(
                            id=m.id,
                            duration=m.duration,
                            resource_requirements=[
                                ResourceRequirementModel(
                                    resource_id=schedule.resources[req.resource].id,
                                    requirement=req.requirement,
                                )
                                for req in m.resource_requirements
                            ],
                        )
                        for m in j.execution_modes
                    ],
                    successor_job_ids=successor_ids[i],
                )
                for i, j in enumerate(schedule.jobs)
            ],
            allocations=allocation_models,
            delay_range_size=schedule.delay_range_size,
            score_hard=schedule.score.hard,
            score_medium=schedule.score.medium,
            score_soft=schedule.score.soft,
        )
