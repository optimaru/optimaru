"""Python reproduction of the Java quickstart `DemoDataGenerator`.

Builds a demo instance of the same scale and distributions: 2 projects (release 0/4,
critical path 10/19), 7 resources (global renewable with capacity 16 + per project one
renewable / two non-renewable), 12 jobs per project (SOURCE + 10 STANDARD + SINK),
STANDARD jobs with 3 execution modes (duration 1..5, demand on 1..3 resources x 1..5),
1 (54%) / 2 (27%) / 3 (19%) successor jobs. Java's java.util.Random stream is not
reproduced — distributions and scale are the reference.
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
    """Pick an index from a cumulative distribution table with one unit random draw (Java's Pair table)."""
    draw = rng.random()
    for i, threshold in enumerate(cumulative):
        if draw <= threshold:
            return i
    return len(cumulative) - 1


def generate_demo_data(seed: int = 0) -> ProjectJobScheduleModel:
    """Build the demo `ProjectJobScheduleModel` (every STANDARD allocation
    uninitialized — the unsolved state; SOURCE/SINK allocations are fixed to the
    zero-duration mode + delay 0)."""
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
        # The resource pool this project draws from: [global, local renewable, local non x2].
        pool = ["0", str(3 * p + 1), str(3 * p + 2), str(3 * p + 3)]
        project_jobs: list[JobModel] = []
        for i in range(JOBS_PER_PROJECT):
            job_type = "SOURCE" if i == 0 else "SINK" if i == JOBS_PER_PROJECT - 1 else "STANDARD"
            if job_type == "STANDARD":
                # The number of required resources is drawn once per job (as in Java).
                requirements_size = rng.randint(1, 3)
                modes = []
                for _ in range(3):
                    requirements: list[ResourceRequirementModel] = []
                    while len(requirements) < requirements_size:
                        # 40% global, 20%/15%/25% local.
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
        # Successors: SOURCE gets the first 3 STANDARD jobs, others get 1 (54%) / 2 (27%) / 3 (19%)
        # later jobs, the last STANDARD gets SINK, SINK gets none (as in Java).
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
            # SOURCE/SINK are fixed to (zero-duration mode, delay 0) — as in the Java demo.
            execution_mode_id=None if job.job_type == "STANDARD" else job.execution_modes[0].id,
            delay=None if job.job_type == "STANDARD" else 0,
        )
        for job in jobs
    ]

    return ProjectJobScheduleModel(projects=projects, resources=resources, jobs=jobs, allocations=allocations)
