"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 Project/Resource/Job(ExecutionMode)/Allocation/ProjectJobSchedule
을 1:1 로 미러링하되, Java quickstart 의 REST 표현처럼 참조는 id 문자열
(`project_id`/`resource_id`/`job_id`/`execution_mode_id`)로 두고, Java 의
Job.successorJobs 처럼 선행 관계도 job 의 `successor_job_ids` 로 둔다. 엔진
변환(`to_core`)이 id 를 인덱스로, successor 리스트를 allocation 의
predecessor 리스트로, (execution_mode_id, delay) 를 합성 planning 값으로
옮긴다::

    from project_job.models import ProjectJobScheduleModel

    model = ProjectJobScheduleModel.model_validate_json(payload)
    schedule = model.to_core()                          # pydantic -> 엔진
    ...solve...
    solved = ProjectJobScheduleModel.from_core(schedule)  # 엔진 -> pydantic

할당(allocation)은 job 과 1:1 이어야 한다 (Java DemoDataGenerator 의 규약).
`allocations[i].execution_mode_id` 가 None 이면 미초기화다 (delay 도 None
이어야 한다); 솔버가 (mode, delay) 를 결정한다. SOURCE/SINK 할당은 항상
pinned 이고, 미초기화로 넘기면 솔버가 (첫 mode, delay 0) 으로 고정한다.
`pydantic` 은 선택 의존성이다 (`pip install project_job[models]`).
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
    """프로젝트. release_date 는 'Total project delay' 제약의 기준
    (critical_path_end = release_date + critical_path_duration)에만 쓰이고,
    Java quickstart 그대로 시작일을 강제하지는 않는다."""

    id: str
    release_date: int = 0
    critical_path_duration: int = Field(default=0, ge=0)


class ResourceModel(_Model):
    """자원. renewable 이면 용량이 '일자별', 아니면 '전체 기간 합' 기준이다.
    project_id None == 글로벌 자원 (Java 의 GlobalResource; 항상 renewable)."""

    id: str
    capacity: int = Field(ge=0)
    renewable: bool = True
    project_id: str | None = None

    @model_validator(mode="after")
    def _global_is_renewable(self):
        if self.project_id is None and not self.renewable:
            raise ValueError("글로벌 자원(project_id None)은 renewable 이어야 한다 (Java GlobalResource)")
        return self


class ResourceRequirementModel(_Model):
    """실행 모드 하나가 자원 하나를 쓰는 양."""

    resource_id: str
    requirement: int = Field(ge=0)


class ExecutionModeModel(_Model):
    """job 의 실행 방식 하나 (기간 + 자원 요구량)."""

    id: str
    duration: int = Field(default=0, ge=0)
    resource_requirements: list[ResourceRequirementModel] = Field(default_factory=list)


class JobModel(_Model):
    """작업. SOURCE/SINK 는 프로젝트의 시작/종료 마일스톤으로, 실행 모드
    하나(기간 0)를 갖는 관례다 (Java quickstart). `successor_job_ids` 가
    선행 관계를 정의한다 (이 job 이 끝나야 후속 job 이 시작 가능)."""

    id: str
    project_id: str
    job_type: str = "STANDARD"
    execution_modes: list[ExecutionModeModel] = Field(default_factory=list)
    successor_job_ids: list[str] = Field(default_factory=list)

    @model_validator(mode="after")
    def _valid(self):
        if self.job_type not in JOB_TYPES:
            raise ValueError(f"job_type 은 {JOB_TYPES} 중 하나여야 한다: {self.job_type!r}")
        if not self.execution_modes:
            raise ValueError(f"job {self.id!r} 은 실행 모드가 최소 1개 필요하다 (SOURCE/SINK 는 기간 0 모드)")
        mode_ids = {m.id for m in self.execution_modes}
        if len(mode_ids) != len(self.execution_modes):
            raise ValueError(f"job {self.id!r} 의 execution mode id 가 중복된다")
        return self


class AllocationModel(_Model):
    """planning entity: job 하나의 할당 (job 과 1:1).
    (execution_mode_id, delay) 가 planning variable 쌍이다 (둘 다 None ==
    미초기화). start_date/end_date 는 읽기 전용 결과 (Java shadow variable)."""

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
            raise ValueError(f"allocation {self.id!r}: execution_mode_id 없이 delay 만 지정할 수 없다")
        return self


class ProjectJobScheduleModel(_Model):
    """planning solution 미러. score_* 는 읽기 전용 결과라 별도 필드로 둔다."""

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
            raise ValueError("project id 가 중복된다")
        if len(resource_ids) != len(self.resources):
            raise ValueError("resource id 가 중복된다")
        if len(job_ids) != len(self.jobs):
            raise ValueError("job id 가 중복된다")
        if len({a.id for a in self.allocations}) != len(self.allocations):
            raise ValueError("allocation id 가 중복된다")
        for resource in self.resources:
            if resource.project_id is not None and resource.project_id not in project_ids:
                raise ValueError(f"resource {resource.id!r} 의 project_id {resource.project_id!r} 가 없다")
        for job in self.jobs:
            if job.project_id not in project_ids:
                raise ValueError(f"job {job.id!r} 의 project_id {job.project_id!r} 가 없다")
            for successor in job.successor_job_ids:
                if successor not in job_ids:
                    raise ValueError(f"job {job.id!r} 의 successor {successor!r} 가 없다")
            for mode in job.execution_modes:
                for req in mode.resource_requirements:
                    if req.resource_id not in resource_ids:
                        raise ValueError(f"mode {mode.id!r} 의 resource_id {req.resource_id!r} 가 없다")
        # 할당은 job 과 1:1 (Java DemoDataGenerator 의 규약).
        allocated_jobs = [a.job_id for a in self.allocations]
        if sorted(allocated_jobs) != sorted(job_ids):
            raise ValueError("allocation 은 모든 job 과 정확히 1:1 이어야 한다")
        for allocation in self.allocations:
            job = next(j for j in self.jobs if j.id == allocation.job_id)
            if allocation.execution_mode_id is not None:
                mode_ids = {m.id for m in job.execution_modes}
                if allocation.execution_mode_id not in mode_ids:
                    raise ValueError(
                        f"allocation {allocation.id!r} 의 execution_mode_id {allocation.execution_mode_id!r} 는 "
                        f"job {job.id!r} 의 모드가 아니다"
                    )
            if allocation.delay is not None and allocation.delay >= self.delay_range_size:
                raise ValueError(f"allocation {allocation.id!r} 의 delay 는 delay_range_size 미만이어야 한다")
        return self

    def to_core(self) -> "_core.ProjectJobSchedule":
        """pydantic -> 엔진 객체: id 참조를 인덱스로, job 의 successor 리스트를
        allocation 의 predecessor 리스트로, (execution_mode_id, delay) 를 합성
        planning 값으로 옮긴다 (선행 그래프에 사이클이 있으면 ValueError).
        score/start/end 필드는 엔진이 다시 계산하므로 무시한다."""
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
                    -1,  # 합성 값은 아래에서 encode_value 로 채운다
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
        """엔진 객체 -> pydantic (현재 배정, 파생 날짜, score 포함). 미초기화
        (-1) 할당은 execution_mode_id/delay None 이 된다. job 의
        successor_job_ids 는 allocation predecessor 리스트를 역변환해 채운다."""
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
