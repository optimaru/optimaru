"""Multi-mode RCPSP formulation for project/job scheduling."""

from __future__ import annotations

from cpsat_plugins import ConstraintConfig
from ortools.sat.python import cp_model
from project_job import calculate_score, constraint_costs
from project_job.constraints import Constraint
from project_job.models import ProjectJobScheduleModel

from .constraints import REGISTRY, ProjectContext

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def solve(
    problem: ProjectJobScheduleModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: list[ConstraintConfig | Constraint | str | dict] | None = None,
    _fixed_modes: list[int] | None = None,
    _fixed_delays: list[int] | None = None,
    _proxy_score: dict[str, int] | None = None,
    _strict_resources: bool = False,
) -> ProjectJobScheduleModel:
    """Solve execution modes, precedence delays and resource capacities."""
    model = cp_model.CpModel()
    objective = REGISTRY.objective(model, ("hard", "medium", "soft"), constraints)
    allocation_count = len(problem.allocations)
    if _fixed_delays is not None and len(_fixed_delays) != allocation_count:
        raise ValueError("_fixed_delays must contain one value per allocation")
    job_by_id = {job.id: job for job in problem.jobs}
    allocation_of_job = {
        allocation.job_id: i for i, allocation in enumerate(problem.allocations)
    }
    jobs = [job_by_id[allocation.job_id] for allocation in problem.allocations]
    # A serial schedule is an upper bound for every renewable-resource
    # instance. The former allocation_count * delay_range_size horizon admitted
    # schedules made only of gratuitous idle time and made the time-indexed
    # resource formulation explode on Patterson/PSPLIB data (Pat1: 7,360
    # ticks instead of 161). Explicitly fixed/pinned delays still extend the
    # bound, preserving evaluation hooks and caller-owned assignments.
    duration_bound = sum(
        max((mode.duration for mode in job.execution_modes), default=0) for job in jobs
    )
    fixed_delay_bound = 0
    for index, allocation in enumerate(problem.allocations):
        if _fixed_delays is not None:
            fixed_delay_bound += _fixed_delays[index]
        elif allocation.pinned and allocation.delay is not None:
            fixed_delay_bound += allocation.delay
    horizon = max(1, duration_bound + fixed_delay_bound)

    mode_vars = {}
    delay = [
        model.new_int_var(
            0,
            min(problem.delay_range_size - 1, horizon),
            f"allocation_{a}_delay",
        )
        for a in range(allocation_count)
    ]
    start = [
        model.new_int_var(0, horizon, f"allocation_{a}_start")
        for a in range(allocation_count)
    ]
    end = [
        model.new_int_var(0, horizon, f"allocation_{a}_end")
        for a in range(allocation_count)
    ]
    intervals_by_resource: dict[str, list[tuple]] = {
        resource.id: [] for resource in problem.resources if resource.renewable
    }
    nonrenewable_terms: dict[str, list] = {
        resource.id: [] for resource in problem.resources if not resource.renewable
    }
    predecessors: list[list[int]] = [[] for _ in range(allocation_count)]
    for source_job in problem.jobs:
        source = allocation_of_job[source_job.id]
        for successor_id in source_job.successor_job_ids:
            predecessors[allocation_of_job[successor_id]].append(source)

    for a, (allocation, job) in enumerate(zip(problem.allocations, jobs)):
        choices = []
        for m, execution_mode in enumerate(job.execution_modes):
            chosen = model.new_bool_var(f"allocation_{a}_mode_{m}")
            mode_vars[a, m] = chosen
            choices.append(chosen)
        model.add_exactly_one(choices)
        delay_var = delay[a]
        start_var = start[a]
        end_var = end[a]
        if _fixed_modes is not None:
            if len(_fixed_modes) != allocation_count:
                raise ValueError("_fixed_modes must contain one value per allocation")
            fixed_mode = _fixed_modes[a]
            if not 0 <= fixed_mode < len(choices):
                raise ValueError("_fixed_modes contains an invalid mode")
            model.add(mode_vars[a, fixed_mode] == 1)
        if _fixed_delays is not None:
            model.add(delay_var == _fixed_delays[a])
        done_var = model.new_int_var(0, horizon, f"allocation_{a}_predecessors_done")
        model.add_max_equality(done_var, [0, *(end[p] for p in predecessors[a])])
        model.add(start_var == done_var + delay_var)
        model.add(
            end_var
            == start_var
            + sum(
                job.execution_modes[m].duration * mode_vars[a, m]
                for m in range(len(job.execution_modes))
            )
        )
        if allocation.pinned:
            if allocation.execution_mode_id is None or allocation.delay is None:
                raise ValueError(
                    f"pinned allocation {allocation.id!r} needs execution_mode_id and delay"
                )
            try:
                pinned_mode = next(
                    i
                    for i, item in enumerate(job.execution_modes)
                    if item.id == allocation.execution_mode_id
                )
            except StopIteration as exc:
                raise ValueError(
                    f"pinned allocation {allocation.id!r} has an invalid mode"
                ) from exc
            model.add(mode_vars[a, pinned_mode] == 1)
            model.add(delay_var == allocation.delay)

        for m, execution_mode in enumerate(job.execution_modes):
            for requirement in execution_mode.resource_requirements:
                if requirement.resource_id in intervals_by_resource:
                    interval = model.new_optional_interval_var(
                        start_var,
                        execution_mode.duration,
                        end_var,
                        mode_vars[a, m],
                        f"allocation_{a}_mode_{m}_{requirement.resource_id}",
                    )
                    intervals_by_resource[requirement.resource_id].append(
                        (
                            interval,
                            requirement.requirement,
                            start_var,
                            end_var,
                            mode_vars[a, m],
                        )
                    )
                elif requirement.resource_id in nonrenewable_terms:
                    nonrenewable_terms[requirement.resource_id].append(
                        requirement.requirement * mode_vars[a, m]
                    )

    project_by_id = {project.id: project for project in problem.projects}
    project_delay = []
    sink_ends = []
    for a, job in enumerate(jobs):
        if job.job_type != "SINK":
            continue
        critical_end = (
            project_by_id[job.project_id].release_date
            + project_by_id[job.project_id].critical_path_duration
        )
        tardiness = model.new_int_var(0, horizon, f"sink_{a}_project_delay")
        model.add_max_equality(tardiness, [0, end[a] - critical_end])
        project_delay.append(tardiness)
        sink_ends.append(end[a])
    makespan = model.new_int_var(0, horizon, "makespan")
    if sink_ends:
        model.add_max_equality(makespan, sink_ends)
    else:
        model.add(makespan == 0)
    REGISTRY.build(
        ProjectContext(
            model,
            problem,
            intervals_by_resource,
            nonrenewable_terms,
            project_delay,
            makespan,
            horizon,
        ),
        objective,
    )
    if _strict_resources:
        # Public RCPSP benchmarks define capacities as mandatory. The normal
        # API keeps the reference model's score semantics (hard violations may exist in
        # an intermediate/final incumbent); this benchmark hook additionally
        # cuts those incumbents from the exact model.
        for resource in problem.resources:
            if resource.renewable:
                entries = intervals_by_resource[resource.id]
                model.add_cumulative(
                    [entry[0] for entry in entries],
                    [entry[1] for entry in entries],
                    resource.capacity,
                )
            else:
                model.add(sum(nonrenewable_terms[resource.id]) <= resource.capacity)

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(
            f"CP-SAT did not find a resource-feasible solution (status={solver.status_name(status)})"
        )
    if _proxy_score is not None:
        _proxy_score.update(objective.score(solver))

    solved = problem.model_copy(deep=True)
    for a, allocation in enumerate(solved.allocations):
        chosen = next(
            m
            for m in range(len(jobs[a].execution_modes))
            if solver.value(mode_vars[a, m])
        )
        allocation.execution_mode_id = jobs[a].execution_modes[chosen].id
        allocation.delay = solver.value(delay[a])
        allocation.start_date = solver.value(start[a])
        allocation.end_date = solver.value(end[a])
    core = solved.to_core()
    calculate_score(core)
    result = ProjectJobScheduleModel.from_core(core)
    score = objective.score_costs(constraint_costs(core))
    result.score_hard = score["hard"]
    result.score_medium = score["medium"]
    result.score_soft = score["soft"]
    return result
