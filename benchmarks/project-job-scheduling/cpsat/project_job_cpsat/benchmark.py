"""Use-case-local engine/CP-SAT comparison.

The formulation, deterministic comparison instances, and CLI entry point live
together in this use-case package.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "project-job-scheduling"
PACKAGE = "project_job"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def _draw_index(rng: random.Random, cumulative: list[float]) -> int:
    draw = rng.random()
    for index, threshold in enumerate(cumulative):
        if draw <= threshold:
            return index
    return len(cumulative) - 1


def _build(project_count: int, jobs_per_project: int, seed: int) -> Any:
    """Structural transcription of the C++ demo's ``generateDemoData``
    (itself the Java generator). Python's RNG stream differs, so the instance
    matches in scale and distribution rather than bit-for-bit."""
    import project_job as pjs

    rng = random.Random(seed)
    projects = [pjs.Project("0", 0, 10), pjs.Project("1", 4, 19)]
    resources = [pjs.Resource("0", 16, True, -1)]
    for p in range(2):
        resources.append(pjs.Resource(str(3 * p + 1), 13 if p == 0 else 24, True, p))
        resources.append(pjs.Resource(str(3 * p + 2), 44 if p == 0 else 66, False, p))
        resources.append(pjs.Resource(str(3 * p + 3), 39 if p == 0 else 56, False, p))
    while len(projects) < project_count:
        index = len(projects)
        projects.append(pjs.Project(str(index), 4 * index, 10 + 5 * index))
        base = len(resources)
        resources.append(pjs.Resource(str(base), 20, True, index))
        resources.append(pjs.Resource(str(base + 1), 50, False, index))
        resources.append(pjs.Resource(str(base + 2), 50, False, index))

    jobs: list[Any] = []
    successors: list[list[int]] = []
    mode_counter = 0
    for p in range(len(projects)):
        base = p * jobs_per_project
        # Resource layout is uniform: index 0 is the global renewable, then
        # each project owns (3p+1) renewable and (3p+2), (3p+3) non-renewable.
        pool = [0, 3 * p + 1, 3 * p + 2, 3 * p + 3]
        for i in range(jobs_per_project):
            job_type = (
                pjs.JobType.SOURCE
                if i == 0
                else pjs.JobType.SINK
                if i == jobs_per_project - 1
                else pjs.JobType.STANDARD
            )
            modes = []
            if job_type != pjs.JobType.STANDARD:
                modes.append(pjs.ExecutionMode(str(mode_counter), 0, []))
                mode_counter += 1
            else:
                requirements_size = rng.randint(1, 3)
                for _ in range(3):
                    chosen: list[Any] = []
                    used: set[int] = set()
                    while len(chosen) < requirements_size:
                        resource = pool[_draw_index(rng, [0.4, 0.6, 0.75, 1.0])]
                        if resource not in used:
                            used.add(resource)
                            chosen.append(
                                pjs.ResourceRequirement(resource, rng.randint(1, 5))
                            )
                    modes.append(
                        pjs.ExecutionMode(str(mode_counter), rng.randint(1, 5), chosen)
                    )
                    mode_counter += 1
            jobs.append(pjs.Job(str(base + i), p, job_type, modes))

        per_project: list[list[int]] = [[] for _ in range(jobs_per_project)]
        per_project[0] = [base + 1, base + 2, base + 3]
        for i in range(1, jobs_per_project):
            count = min(
                _draw_index(rng, [0.54, 0.81, 1.0]) + 1, jobs_per_project - i - 1
            )
            picked: list[int] = []
            while len(picked) < count:
                candidate = base + rng.randint(i + 1, jobs_per_project - 1)
                if candidate not in picked:
                    picked.append(candidate)
            per_project[i] = sorted(picked)
        per_project[jobs_per_project - 2] = [base + jobs_per_project - 1]
        per_project[jobs_per_project - 1] = []
        successors.extend(per_project)

    predecessors: list[list[int]] = [[] for _ in jobs]
    for a, targets in enumerate(successors):
        for target in targets:
            predecessors[target].append(a)

    allocations = [
        pjs.Allocation(
            str(a),
            a,
            predecessors[a],
            0 if jobs[a].job_type != pjs.JobType.STANDARD else -1,
        )
        for a in range(len(jobs))
    ]
    return pjs.ProjectJobSchedule(projects, resources, jobs, allocations)


def instances() -> dict[str, Callable[[], Any]]:
    return {
        "demo": lambda: _build(2, 12, seed=0),
        "medium": lambda: _build(4, 18, seed=1),
        "large": lambda: _build(6, 26, seed=2),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.allocations)} jobs / {len(problem.projects)} projects / {len(problem.resources)} res"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import project_job as pjs

    config = pjs.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    pjs.Solver(config).solve(problem)


def self_report(problem: Any) -> tuple[float, ...]:
    """The engine's own score, folded the way the package's scorer reports it.

    `problem.score` exposes three levels, but `project_job.constraints.score`
    returns the engine's two: medium rides in soft at 1e9 (see the package
    docstring). The crosscheck compares like with like, so fold here rather
    than let a three-vs-two comparison silently truncate.
    """
    score = problem.score
    return (score.hard, score.medium * 10**9 + score.soft)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from project_job.models import ProjectJobScheduleModel

    from project_job_cpsat import solve

    solved = solve(
        ProjectJobScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()



# --- Timefold quickstart (benchmarks/timefold-quickstarts) ---------------------

TIMEFOLD = "project-job-scheduling"


def to_timefold(problem: Any) -> dict:
    projects, resources, jobs, allocations = problem.projects, problem.resources, problem.jobs, problem.allocations
    allocation_of_job = {a.job: i for i, a in enumerate(allocations)}
    successors: list[list[int]] = [[] for _ in allocations]
    for i, allocation in enumerate(allocations):
        for predecessor in allocation.predecessor_allocations:
            successors[predecessor].append(i)

    def boundary(project: int, kind: str) -> str:
        return next(
            allocations[allocation_of_job[j]].id
            for j, job in enumerate(jobs)
            if job.project == project and job.job_type.name == kind
        )

    def resource(r: Any) -> dict:
        if r.project < 0:
            return {"@type": "global", "id": r.id, "capacity": r.capacity}
        return {
            "@type": "local",
            "id": r.id,
            "capacity": r.capacity,
            "project": projects[r.project].id,
            "renewable": r.renewable,
        }

    job_json = []
    for j, job in enumerate(jobs):
        job_json.append(
            {
                "id": job.id,
                "project": projects[job.project].id,
                "jobType": job.job_type.name,
                "executionModes": [
                    {
                        "id": mode.id,
                        "job": job.id,
                        "duration": mode.duration,
                        "resourceRequirements": [
                            {
                                "id": f"{mode.id}-{k}",
                                "executionMode": mode.id,
                                "resource": resources[req.resource].id,
                                "requirement": req.requirement,
                            }
                            for k, req in enumerate(mode.resource_requirements)
                        ],
                    }
                    for mode in job.execution_modes
                ],
                "successorJobs": [jobs[allocations[s].job].id for s in successors[allocation_of_job[j]]],
            }
        )
    allocation_json = []
    for i, a in enumerate(allocations):
        assigned = a.mode_delay >= 0
        job = jobs[a.job]
        allocation_json.append(
            {
                "id": a.id,
                "job": job.id,
                "pinned": a.pinned,
                "sourceAllocation": boundary(job.project, "SOURCE"),
                "sinkAllocation": boundary(job.project, "SINK"),
                "predecessorAllocations": [allocations[p].id for p in a.predecessor_allocations],
                "successorAllocations": [allocations[s].id for s in successors[i]],
                "executionMode": job.execution_modes[problem.execution_mode_index_of_value(i, a.mode_delay)].id
                if assigned
                else None,
                "delay": problem.delay_of_value(a.mode_delay) if assigned else None,
            }
        )
    return {
        "projects": [
            {"id": p.id, "releaseDate": p.release_date, "criticalPathDuration": p.critical_path_duration}
            for p in projects
        ],
        "resources": [resource(r) for r in resources],
        "jobs": job_json,
        "allocations": allocation_json,
    }


def from_timefold(problem: Any, result: dict) -> None:
    jobs = problem.jobs
    chosen = result["assignment"]["Allocation"]
    assignment = []
    for a in problem.allocations:
        variables = chosen[a.id]
        if variables["executionMode"] is None:
            assignment.append(-1)
            continue
        slot = [mode.id for mode in jobs[a.job].execution_modes].index(variables["executionMode"])
        assignment.append(problem.encode_value(int(variables["delay"] or 0), slot))
    problem.set_assignment(assignment)


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
