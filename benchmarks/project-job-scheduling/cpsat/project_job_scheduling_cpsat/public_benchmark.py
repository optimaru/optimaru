"""Compare both backends on public RCPSP instances in Patterson format, or on generated ones of that shape."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import random
import sys
import time
import urllib.request
import zipfile
from collections import deque
from dataclasses import dataclass
from pathlib import Path

from external_solver.project_job_scheduling import solve as solve_cpsat

import optimaru.project_job_scheduling as pjs
from cpsat_plugins.results import write_results, write_whole
from optimaru.project_job_scheduling.models import (
    AllocationModel,
    ExecutionModeModel,
    JobModel,
    ProjectJobScheduleModel,
    ProjectModel,
    ResourceModel,
    ResourceRequirementModel,
)

# Pat1-Pat5 of the RG30 set (Vanhoucke et al.), 30 activities and 4 renewable
# resources each, in Patterson format. The original RCPLIB release is an 86 MB
# archive whose older versions are withdrawn, so the benchmark reads this small
# third-party re-packaging of the five instances instead. That archive is
# rebuilt from time to time (its own SHA-256 changes), so each instance file
# is pinned rather than the archive.
ARCHIVE_URL = "https://www.hexaly.com/examples/rcpsp.zip"
ARCHIVE_PREFIX = "rcpsp/instances/"
INSTANCE_SHA256 = {
    "Pat1": "97983fd442b705d6f19604c78915d092b81f051c3ee4038a8e271a04a98bab52",
    "Pat2": "4b65043f5d3c49feb6bc2137f08ee1b045abf30b9bd09c522357162e116e54dd",
    "Pat3": "a753b4fdcab89fd3248bec904f10997cdd9b64b9b56a054a2e0a274221326586",
    "Pat4": "ebe41524472f3158942bd726ed9c698d19f185c249145ecd204a41481281f800",
    "Pat5": "a2f1a250fb2e9cd0ee7e94737e595ca765b357a6a5af4a5a5b20ba61600cb31d",
}
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


def _verified(payload: bytes) -> bool:
    """Whether the archive holds the five pinned instance files, byte for byte.

    Whatever else a rebuilt archive carries (its sample programs, a further
    instance) is not read and does not matter.
    """
    try:
        with zipfile.ZipFile(io.BytesIO(payload)) as archive:
            found = {
                Path(item).stem: hashlib.sha256(archive.read(item)).hexdigest()
                for item in archive.namelist()
                if item.startswith(ARCHIVE_PREFIX) and item.endswith(".rcp")
            }
    except zipfile.BadZipFile:
        return False
    return all(found.get(name) == digest for name, digest in INSTANCE_SHA256.items())


def _download(cache_dir: Path) -> bytes:
    archive = cache_dir / "rcpsp-rg30-pat1-5.zip"
    if archive.exists():
        payload = archive.read_bytes()
        if _verified(payload):
            return payload
    request = urllib.request.Request(ARCHIVE_URL, headers={"User-Agent": "optimaru-benchmark/1.0"})
    with urllib.request.urlopen(request, timeout=60) as response:
        payload = response.read()
    if not _verified(payload):
        # Nothing is cached: a transient error page must not stick.
        raise ValueError(f"{archive.name}: the downloaded archive does not hold the pinned instance files")
    write_whole(archive, payload)
    return payload


def instance_names() -> list[str]:
    """Every instance of the archive, in name order."""
    return sorted(INSTANCE_SHA256)


def load_instance(name: str, cache_dir: Path) -> RcpspInstance:
    # Only a pinned file is read: an archive may carry others, and nothing vouches for them.
    if name not in INSTANCE_SHA256:
        raise ValueError(f"unknown instance {name!r}; available: {', '.join(instance_names())}")
    with zipfile.ZipFile(io.BytesIO(_download(cache_dir))) as archive:
        return parse_instance(archive.read(f"{ARCHIVE_PREFIX}{name}.rcp").decode(), name=name)


def synthetic_instance(activity_count: int, seed: int = 0) -> RcpspInstance:
    """A seeded instance in the shape of the RG30 set: `activity_count` activities between a
    source and a sink of duration 0, over four renewable resources of capacity 10.

    An activity lasts 1 to 10 time units and uses three of the four resources, 1 to 8 units of
    each, so the resources bind before the precedences do: the activities' work is about twice
    what the longest chain takes. The network is wide and shallow, as in RG30: the activities
    are dealt into one layer per eight of them (three layers at least, one per activity below three), and an activity follows
    one or two activities of the layer before it and, three times in ten, one of an earlier
    layer. The first layer follows the source; what nothing follows precedes the sink.
    """
    if activity_count < 1:
        raise ValueError("a synthetic instance needs at least one activity")
    rng = random.Random(seed)
    resource_count = 4
    capacities = (10,) * resource_count
    durations = [rng.randint(1, 10) for _ in range(activity_count)]
    requirements = []
    for _ in range(activity_count):
        used = rng.sample(range(resource_count), 3)
        requirements.append(tuple(rng.randint(1, 8) if resource in used else 0 for resource in range(resource_count)))

    # Layers, in order: every layer has an activity, the others land anywhere.
    layer_count = min(activity_count, max(3, activity_count // 8))
    layer_of = sorted([*range(layer_count), *(rng.randrange(layer_count) for _ in range(activity_count - layer_count))])
    members: list[list[int]] = [[] for _ in range(layer_count)]
    for activity, layer in enumerate(layer_of):
        members[layer].append(activity)
    predecessors: list[list[int]] = []
    for activity, layer in enumerate(layer_of):
        if layer == 0:
            predecessors.append([])
            continue
        before = members[layer - 1]
        chosen = rng.sample(before, min(len(before), rng.randint(1, 2)))
        earlier = [other for other in range(activity) if layer_of[other] < layer - 1]
        if earlier and rng.random() < 0.3:
            chosen.append(rng.choice(earlier))
        predecessors.append(sorted(chosen))
    successors: list[list[int]] = [[] for _ in range(activity_count)]
    for activity, before in enumerate(predecessors):
        for predecessor in before:
            successors[predecessor].append(activity)

    # Task 0 is the source, tasks 1..n the activities, task n + 1 the sink.
    idle = (0,) * resource_count
    sink = activity_count + 1
    tasks = [Task(0, idle, tuple(1 + activity for activity in range(activity_count) if not predecessors[activity]))]
    for activity in range(activity_count):
        after = tuple(1 + successor for successor in sorted(successors[activity])) or (sink,)
        tasks.append(Task(durations[activity], requirements[activity], after))
    tasks.append(Task(0, idle, ()))
    return RcpspInstance(f"synthetic-{activity_count}-s{seed}", capacities, tuple(tasks))


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
                            ResourceRequirementModel(resource_id=resource_ids[r], requirement=requirement)
                            for r, requirement in enumerate(task.requirements)
                            if requirement
                        ],
                    )
                ],
                successor_job_ids=[f"J{successor + 1}" for successor in task.successors],
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


def rescore(instance: RcpspInstance, solved: ProjectJobScheduleModel) -> tuple[int, int]:
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
                task.requirements[resource] for i, task in enumerate(instance.tasks) if starts[i] <= day < ends[i]
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
        # The engine runs one start here, on one thread; `--workers` is CP-SAT's.
        solved = pjs.solve(
            model,
            time_limit_seconds=seconds,
            random_seed=seed,
            parallel_solver_count=1,
        )
    else:
        solved = solve_cpsat(
            model,
            time_limit_seconds=seconds,
            random_seed=seed,
            parallel_solver_count=workers,
            _strict_resources=True,
        )
    return solved, time.perf_counter() - started


SOLVER_NAMES = {"engine": "engine", "cpsat": "cp-sat"}


def run_case(
    instance: RcpspInstance,
    *,
    seconds: float,
    seed: int,
    workers: int,
    backends: tuple[str, ...],
    log=print,
) -> dict:
    """Solve one instance with every backend and re-verify each schedule.

    ``makespan`` stays None when a backend returns nothing in time (``status``
    says so); ``feasible`` is False when the schedule exceeds a resource.
    """
    model = make_model(instance)
    log(
        f"{instance.name}: {len(instance.tasks)} tasks, "
        f"{len(instance.capacities)} resources, critical path {critical_path(instance)}"
    )
    runs = []
    for backend in backends:
        run = {
            "solver": SOLVER_NAMES[backend],
            "makespan": None,
            "resource_excess": None,
            "feasible": False,
            "seconds": None,
            # ok, no solution, infeasible or mismatch; `status` has the words.
            "outcome": "ok",
            "status": "",
        }
        runs.append(run)
        started = time.perf_counter()
        try:
            solved, elapsed = _run(backend, model, seconds=seconds, seed=seed, workers=workers)
        except RuntimeError as exc:
            run["seconds"] = time.perf_counter() - started
            run["outcome"] = "no solution"
            run["status"] = f"no solution: {exc}"
            log(f"{backend:6} no-solution within {seconds:.2f}s: {exc}")
            continue
        makespan, resource_excess = rescore(instance, solved)
        if solved.score_hard != -resource_excess:
            # A harness or adapter bug, not a result. Recorded, so a run of several
            # instances keeps what it has measured; the publisher refuses it.
            run["seconds"] = elapsed
            run["outcome"] = "mismatch"
            run["status"] = (
                f"mismatch: the solver reports a hard score of {solved.score_hard}, "
                f"the independent scorer a resource excess of {resource_excess}"
            )
            log(f"{backend:6} {run['status']}")
            continue
        run.update(
            outcome="ok" if resource_excess == 0 else "infeasible",
            makespan=makespan,
            resource_excess=resource_excess,
            feasible=resource_excess == 0,
            seconds=elapsed,
        )
        log(
            f"{backend:6} makespan={makespan} resource_excess={resource_excess} "
            f"feasible={resource_excess == 0} elapsed={elapsed:.2f}s "
            f"score={solved.score_hard}/{solved.score_medium}/{solved.score_soft}"
        )
    return {
        "instance": instance.name,
        "tasks": len(instance.tasks),
        "resources": len(instance.capacities),
        "critical_path": critical_path(instance),
        # What the run was given, so a table can state it and a publisher can check it.
        "budget": seconds,
        "seed": seed,
        "workers": workers,
        "runs": runs,
    }


def _seed_list(text: str) -> list[int]:
    """`--synthetic-seeds`: comma-separated whole numbers."""
    try:
        return [int(seed) for seed in text.split(",")]
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a comma-separated list of seeds: {text!r}") from None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--instance",
        action="append",
        default=None,
        help=f"instance stem; repeat to select several (default {DEFAULT_INSTANCE})",
    )
    parser.add_argument("--all", action="store_true", help="run every instance of the archive")
    parser.add_argument(
        "--synthetic",
        type=int,
        action="append",
        default=None,
        metavar="N",
        help="solve generated instances with N activities instead; repeat for several sizes",
    )
    parser.add_argument(
        "--synthetic-seeds",
        type=_seed_list,
        default=None,
        help="comma-separated seeds of the generated instances: one instance per size and seed (default 0)",
    )
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument(
        "--workers",
        type=int,
        default=1,
        help="CP-SAT search workers; the engine runs one start here, on one thread",
    )
    parser.add_argument("--backend", choices=("engine", "cpsat", "both"), default="both")
    parser.add_argument("--cache-dir", type=Path, default=Path(__file__).with_name(".cache"))
    parser.add_argument(
        "--json",
        action="store_true",
        dest="as_json",
        help="print the results as JSON (progress goes to stderr)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="write the results as JSON here after every instance, so an interrupted run keeps them",
    )
    args = parser.parse_args(argv)
    if sum(bool(choice) for choice in (args.all, args.instance, args.synthetic)) > 1:
        parser.error("--all, --instance and --synthetic are mutually exclusive")
    if args.synthetic_seeds and not args.synthetic:
        parser.error("--synthetic-seeds goes with --synthetic")

    if args.synthetic:
        seeds = args.synthetic_seeds or [0]
        instances = [synthetic_instance(count, seed) for count in args.synthetic for seed in seeds]
    else:
        names = instance_names() if args.all else (args.instance or [DEFAULT_INSTANCE])
        instances = [load_instance(name, args.cache_dir) for name in names]
    backends = ("engine", "cpsat") if args.backend == "both" else (args.backend,)

    def log(line: str) -> None:
        print(line, file=sys.stderr if args.as_json else sys.stdout, flush=True)

    results = []
    for instance in instances:
        results.append(
            run_case(
                instance,
                seconds=args.seconds,
                seed=args.seed,
                workers=args.workers,
                backends=backends,
                log=log,
            )
        )
        if args.out is not None:
            write_results(args.out, results)
    if args.as_json:
        print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
