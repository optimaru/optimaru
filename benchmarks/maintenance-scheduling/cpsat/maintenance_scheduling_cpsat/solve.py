"""CP-SAT formulation matching maintenance scheduling's pairwise score."""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from datetime import date, timedelta

from ortools.sat.python import cp_model

from maintenance_scheduling import calculate_score, constraint_costs
from maintenance_scheduling.constraints import Constraint
from maintenance_scheduling.models import MaintenanceScheduleModel
from cpsat_plugins import ConstraintConfig

from .constraints import MaintenanceContext, REGISTRY

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def _end_date(start: date, business_days: int) -> date:
    result = start
    added = 0
    while added < business_days:
        if result.weekday() < 5:
            added += 1
        result += timedelta(days=1)
    return result


def solve(
    problem: MaintenanceScheduleModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
    _fixed_options: list[tuple[int, int]] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> MaintenanceScheduleModel:
    """Solve all crew/start-date choices and return the shared model type."""
    if problem.jobs and not problem.crews:
        raise ValueError("at least one crew is required when jobs exist")
    dates: list[date] = []
    current = problem.from_date
    while current < problem.to_date:
        if current.weekday() < 5:
            dates.append(current)
        current += timedelta(days=1)
    if problem.jobs and not dates:
        raise ValueError("the planning window contains no business day")

    model = cp_model.CpModel()
    crew_count, date_count = len(problem.crews), len(dates)
    options = [(crew, day) for crew in range(crew_count) for day in range(date_count)]
    x = {
        (job, crew, day): model.new_bool_var(f"job_{job}_crew_{crew}_day_{day}")
        for job in range(len(problem.jobs))
        for crew, day in options
    }
    for job_index, job in enumerate(problem.jobs):
        model.add_exactly_one(x[job_index, crew, day] for crew, day in options)
        if _fixed_options is not None:
            if len(_fixed_options) != len(problem.jobs):
                raise ValueError("_fixed_options must contain one value per job")
            crew, day = _fixed_options[job_index]
            if (crew, day) not in options:
                raise ValueError("_fixed_options contains an invalid option")
            model.add(x[job_index, crew, day] == 1)
        if job.pinned:
            if job.crew_id is None or job.start_date is None:
                raise ValueError(f"pinned job {job.id!r} needs crew_id and start_date")
            try:
                pinned_crew = next(
                    i for i, crew in enumerate(problem.crews) if crew.id == job.crew_id
                )
                pinned_day = dates.index(job.start_date)
            except (StopIteration, ValueError) as exc:
                raise ValueError(
                    f"pinned job {job.id!r} has an invalid assignment"
                ) from exc
            model.add(x[job_index, pinned_crew, pinned_day] == 1)

    ends = [
        [_end_date(day, job.duration_in_days) for day in dates] for job in problem.jobs
    ]
    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)
    REGISTRY.build(
        MaintenanceContext(model, problem, dates, options, x, ends), objective
    )

    objective.minimize()

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = solver.solve(model)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(
            f"CP-SAT did not find a solution (status={solver.status_name(status)})"
        )
    if _proxy_score is not None:
        _proxy_score.update(objective.score(solver))

    solved = problem.model_copy(deep=True)
    for j, job in enumerate(solved.jobs):
        crew, day = next((c, d) for c, d in options if solver.value(x[j, c, d]))
        job.crew_id = solved.crews[crew].id
        job.start_date = dates[day]
        job.end_date = ends[j][day]
    core = solved.to_core()
    calculate_score(core)
    result = MaintenanceScheduleModel.from_core(core)
    selected_score = objective.score_costs(constraint_costs(core))
    result.score_hard = selected_score["hard"]
    result.score_soft = float(selected_score["soft"])
    return result
