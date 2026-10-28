"""Compare both backends on Hexaly's public Patterson RCPSP bundle."""

from __future__ import annotations

import argparse
import hashlib
import io
import time
import urllib.request
import zipfile
from collections import deque
from dataclasses import dataclass
from pathlib import Path

import project_job as pjs
from project_job.models import (
    AllocationModel,
    ExecutionModeModel,
    JobModel,
    ProjectJobScheduleModel,
    ProjectModel,
    ResourceModel,
    ResourceRequirementModel,
)

from .solve import solve as solve_cpsat

ARCHIVE_URL = "https://www.hexaly.com/examples/rcpsp.zip"
ARCHIVE_SHA256 = "71c5012250ed5e2a7c02da701211144bb0a2b22935ae94e4a02bbb8d06b9f17b"
DEFAULT_INSTANCE = "Pat1"


@dataclass(frozen=True)
class Task:
    duration: int
    requirements: tuple[int, ...]
    successors: tuple[int, ...]


@dataclass(frozen=True)
class RcpspInstance:
    name: str
    capacities: tuple[int, ...]
    tasks: tuple[Task, ...]


def parse_instance(text: str, *, name: str = "instance") -> RcpspInstance:
    lines = [line.split() for line in text.splitlines() if line.strip()]
    if len(lines) < 3:
        raise ValueError("incomplete Patterson RCPSP instance")
    task_count, resource_count = map(int, lines[0])
    capacities = tuple(map(int, lines[1]))
    if len(capacities) != resource_count or len(lines[2:]) != task_count:
        raise ValueError("RCPSP header counts do not match the body")
    tasks: list[Task] = []
    for index, row in enumerate(lines[2:]):
        values = list(map(int, row))
        duration = values[0]
        requirements = tuple(values[1 : 1 + resource_count])
        successor_count = values[1 + resource_count]
        successors = tuple(value - 1 for value in values[2 + resource_count :])
        if len(successors) != successor_count:
            raise ValueError(f"task {index + 1} successor count mismatch")
        if any(successor < 0 or successor >= task_count for successor in successors):
            raise ValueError(f"task {index + 1} has an invalid successor")
        tasks.append(Task(duration, requirements, successors))
    return RcpspInstance(name, capacities, tuple(tasks))


def _download(cache_dir: Path) -> bytes:
    cache_dir.mkdir(parents=True, exist_ok=True)
    archive = cache_dir / "rcpsp.zip"
    if not archive.exists():
        with urllib.request.urlopen(ARCHIVE_URL, timeout=60) as response:
            archive.write_bytes(response.read())
    payload = archive.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if digest != ARCHIVE_SHA256:
        raise ValueError(f"rcpsp.zip checksum mismatch: {digest}")
    return payload


def load_instance(name: str, cache_dir: Path) -> RcpspInstance:
    with zipfile.ZipFile(io.BytesIO(_download(cache_dir))) as archive:
        member = f"rcpsp/instances/{name}.rcp"
        try:
            return parse_instance(archive.read(member).decode(), name=name)
        except KeyError as exc:
            available = sorted(
                Path(item).stem
                for item in archive.namelist()
                if item.startswith("rcpsp/instances/") and item.endswith(".rcp")
            )
            raise ValueError(
                f"unknown instance {name!r}; available: {', '.join(available)}"
            ) from exc


def _topological_order(instance: RcpspInstance) -> list[int]:
    indegree = [0] * len(instance.tasks)
    for task in instance.tasks:
        for successor in task.successors:
            indegree[successor] += 1
    queue = deque(i for i, degree in enumerate(indegree) if degree == 0)
    order: list[int] = []
    while queue:
        task = queue.popleft()
        order.append(task)
        for successor in instance.tasks[task].successors:
            indegree[successor] -= 1
            if indegree[successor] == 0:
                queue.append(successor)
    if len(order) != len(instance.tasks):
        raise ValueError("RCPSP precedence graph contains a cycle")
    return order


def critical_path(instance: RcpspInstance) -> int:
    earliest_end = [0] * len(instance.tasks)
    for task_index in _topological_order(instance):
        end = earliest_end[task_index] + instance.tasks[task_index].duration
        for successor in instance.tasks[task_index].successors:
            earliest_end[successor] = max(earliest_end[successor], end)
    return max(earliest_end[i] + task.duration for i, task in enumerate(instance.tasks))


def make_model(instance: RcpspInstance) -> ProjectJobScheduleModel:
    project_id = instance.name
    resource_ids = [f"R{i + 1}" for i in range(len(instance.capacities))]
    jobs: list[JobModel] = []
    allocations: list[AllocationModel] = []
    last = len(instance.tasks) - 1
    for index, task in enumerate(instance.tasks):
        job_type = "SOURCE" if index == 0 else "SINK" if index == last else "STANDARD"
        mode_id = f"M{index + 1}"
        jobs.append(
            JobModel(
                id=f"J{index + 1}",
                project_id=project_id,
                job_type=job_type,
                execution_modes=[
                    ExecutionModeModel(
                        id=mode_id,
                        duration=task.duration,
                        resource_requirements=[
                            ResourceRequirementModel(
                                resource_id=resource_ids[r], requirement=requirement
                            )
                            for r, requirement in enumerate(task.requirements)
                            if requirement
                        ],
                    )
                ],
                successor_job_ids=[
                    f"J{successor + 1}" for successor in task.successors
                ],
            )
        )
        pinned = job_type != "STANDARD"
        allocations.append(
            AllocationModel(
                id=f"A{index + 1}",
                job_id=f"J{index + 1}",
                execution_mode_id=mode_id if pinned else None,
                delay=0 if pinned else None,
                pinned=pinned,
            )
        )
    horizon = sum(task.duration for task in instance.tasks)
    return ProjectJobScheduleModel(
        projects=[
            ProjectModel(
                id=project_id,
                release_date=0,
                critical_path_duration=critical_path(instance),
            )
        ],
        resources=[
            ResourceModel(id=resource_id, capacity=capacity)
            for resource_id, capacity in zip(resource_ids, instance.capacities)
        ],
        jobs=jobs,
        allocations=allocations,
        delay_range_size=max(1, horizon + 1),
    )


def rescore(
    instance: RcpspInstance, solved: ProjectJobScheduleModel
) -> tuple[int, int]:
    """Independently verify durations, precedences and cumulative resources."""
    allocations = {allocation.job_id: allocation for allocation in solved.allocations}
    starts: list[int] = []
    ends: list[int] = []
    for i, task in enumerate(instance.tasks):
        allocation = allocations[f"J{i + 1}"]
        if allocation.start_date is None or allocation.end_date is None:
            raise ValueError(f"task {i + 1} has no scheduled dates")
        if allocation.end_date - allocation.start_date != task.duration:
            raise ValueError(f"task {i + 1} duration mismatch")
        starts.append(allocation.start_date)
        ends.append(allocation.end_date)
    for i, task in enumerate(instance.tasks):
        for successor in task.successors:
            if starts[successor] < ends[i]:
                raise ValueError(f"precedence {i + 1}->{successor + 1} violated")
    resource_excess = 0
    for resource, capacity in enumerate(instance.capacities):
        for day in range(max(ends, default=0)):
            usage = sum(
                task.requirements[resource]
                for i, task in enumerate(instance.tasks)
                if starts[i] <= day < ends[i]
            )
            resource_excess += max(0, usage - capacity)
    return max(ends, default=0), resource_excess


def _run(
    backend: str,
    model: ProjectJobScheduleModel,
    *,
    seconds: float,
    seed: int,
    workers: int,
) -> tuple[ProjectJobScheduleModel, float]:
    started = time.perf_counter()
    if backend == "engine":
        solved = pjs.solve(
            model,
            time_limit_seconds=seconds,
            random_seed=seed,
            parallel_solver_count=workers,
        )
    else:
        solved = solve_cpsat(
            model,
            time_limit_seconds=seconds,
            random_seed=seed,
            num_workers=workers,
            _strict_resources=True,
        )
    return solved, time.perf_counter() - started


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", default=DEFAULT_INSTANCE)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument(
        "--backend", choices=("engine", "cpsat", "both"), default="both"
    )
    parser.add_argument(
        "--cache-dir", type=Path, default=Path(__file__).with_name(".cache")
    )
    args = parser.parse_args(argv)

    instance = load_instance(args.instance, args.cache_dir)
    model = make_model(instance)
    print(
        f"{instance.name}: {len(instance.tasks)} tasks, "
        f"{len(instance.capacities)} resources, critical path {critical_path(instance)}"
    )
    backends = ("engine", "cpsat") if args.backend == "both" else (args.backend,)
    for backend in backends:
        try:
            solved, elapsed = _run(
                backend,
                model,
                seconds=args.seconds,
                seed=args.seed,
                workers=args.workers,
            )
        except RuntimeError as exc:
            print(f"{backend:6} no-solution within {args.seconds:.2f}s: {exc}")
            continue
        makespan, resource_excess = rescore(instance, solved)
        if solved.score_hard != -resource_excess:
            raise ValueError("solver report and independent resource score disagree")
        print(
            f"{backend:6} makespan={makespan} resource_excess={resource_excess} "
            f"feasible={resource_excess == 0} elapsed={elapsed:.2f}s "
            f"score={solved.score_hard}/{solved.score_medium}/{solved.score_soft}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
