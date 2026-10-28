"""Java quickstart `DemoDataGenerator` 의 Python 재현.

같은 규모/분포의 데모 인스턴스를 만든다: 프로젝트 2개 (release 0/4,
critical path 10/19), 자원 7개 (글로벌 renewable 용량 16 + 프로젝트별
renewable 1개 / non-renewable 2개), 프로젝트당 잡 12개 (SOURCE + STANDARD
10 + SINK), STANDARD 잡은 실행 모드 3개 (기간 1..5, 자원 요구 1..3종 x
1..5), 후행 잡 1(54%)/2(27%)/3(19%)개. Java 의 java.util.Random 스트림까지
재현하지는 않는다 — 분포와 규모가 기준이다.
"""

import random

from .models import (
    AllocationModel,
    ExecutionModeModel,
    JobModel,
    ProjectJobScheduleModel,
    ProjectModel,
    ResourceModel,
    ResourceRequirementModel,
)

__all__ = ["generate_demo_data"]

JOBS_PER_PROJECT = 12


def _draw_index(rng: random.Random, cumulative: list[float]) -> int:
    """단위 난수 하나로 누적 분포표의 인덱스를 뽑는다 (Java 의 Pair 테이블)."""
    draw = rng.random()
    for i, threshold in enumerate(cumulative):
        if draw <= threshold:
            return i
    return len(cumulative) - 1


def generate_demo_data(seed: int = 0) -> ProjectJobScheduleModel:
    """데모 `ProjectJobScheduleModel` 을 만든다 (STANDARD 할당은 전부
    미초기화 — 풀기 전 상태; SOURCE/SINK 할당은 기간 0 모드 + delay 0 으로
    고정된다)."""
    rng = random.Random(seed)

    projects = [
        ProjectModel(id="0", release_date=0, critical_path_duration=10),
        ProjectModel(id="1", release_date=4, critical_path_duration=19),
    ]
    resources = [
        ResourceModel(id="0", capacity=16, renewable=True),
        ResourceModel(id="1", capacity=13, renewable=True, project_id="0"),
        ResourceModel(id="2", capacity=44, renewable=False, project_id="0"),
        ResourceModel(id="3", capacity=39, renewable=False, project_id="0"),
        ResourceModel(id="4", capacity=24, renewable=True, project_id="1"),
        ResourceModel(id="5", capacity=66, renewable=False, project_id="1"),
        ResourceModel(id="6", capacity=56, renewable=False, project_id="1"),
    ]

    jobs: list[JobModel] = []
    mode_counter = 0
    for p, project in enumerate(projects):
        base = p * JOBS_PER_PROJECT
        # 이 프로젝트가 뽑는 자원 풀: [글로벌, 로컬 renewable, 로컬 non x2].
        pool = ["0", str(3 * p + 1), str(3 * p + 2), str(3 * p + 3)]
        project_jobs: list[JobModel] = []
        for i in range(JOBS_PER_PROJECT):
            job_type = "SOURCE" if i == 0 else "SINK" if i == JOBS_PER_PROJECT - 1 else "STANDARD"
            if job_type == "STANDARD":
                # 요구 자원 수는 잡당 한 번 뽑는다 (Java 그대로).
                requirements_size = rng.randint(1, 3)
                modes = []
                for _ in range(3):
                    requirements: list[ResourceRequirementModel] = []
                    while len(requirements) < requirements_size:
                        # 40% 글로벌, 20%/15%/25% 로컬.
                        resource_id = pool[_draw_index(rng, [0.4, 0.6, 0.75, 1.0])]
                        if all(req.resource_id != resource_id for req in requirements):
                            requirements.append(
                                ResourceRequirementModel(resource_id=resource_id, requirement=rng.randint(1, 5))
                            )
                    modes.append(
                        ExecutionModeModel(
                            id=str(mode_counter), duration=rng.randint(1, 5), resource_requirements=requirements
                        )
                    )
                    mode_counter += 1
            else:
                modes = [ExecutionModeModel(id=str(mode_counter), duration=0)]
                mode_counter += 1
            project_jobs.append(
                JobModel(id=str(base + i), project_id=project.id, job_type=job_type, execution_modes=modes)
            )
        # 후행 잡: SOURCE 는 첫 STANDARD 3개, 그 외에는 1(54%)/2(27%)/3(19%)
        # 개의 뒤쪽 잡, 마지막 STANDARD 는 SINK, SINK 는 없음 (Java 그대로).
        project_jobs[0].successor_job_ids = [str(base + 1), str(base + 2), str(base + 3)]
        for i in range(1, JOBS_PER_PROJECT):
            count = min(_draw_index(rng, [0.54, 0.81, 1.0]) + 1, JOBS_PER_PROJECT - i - 1)
            successors: list[str] = []
            while len(successors) < count:
                job_id = str(base + rng.randint(i + 1, JOBS_PER_PROJECT - 1))
                if job_id not in successors:
                    successors.append(job_id)
            project_jobs[i].successor_job_ids = sorted(successors, key=int)
        project_jobs[JOBS_PER_PROJECT - 2].successor_job_ids = [str(base + JOBS_PER_PROJECT - 1)]
        project_jobs[JOBS_PER_PROJECT - 1].successor_job_ids = []
        jobs.extend(project_jobs)

    allocations = [
        AllocationModel(
            id=job.id,
            job_id=job.id,
            # SOURCE/SINK 는 (기간 0 모드, delay 0) 고정 — Java 데모 그대로.
            execution_mode_id=None if job.job_type == "STANDARD" else job.execution_modes[0].id,
            delay=None if job.job_type == "STANDARD" else 0,
        )
        for job in jobs
    ]

    return ProjectJobScheduleModel(projects=projects, resources=resources, jobs=jobs, allocations=allocations)
