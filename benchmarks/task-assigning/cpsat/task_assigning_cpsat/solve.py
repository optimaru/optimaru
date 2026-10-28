"""Optional-circuit CP-SAT formulation for task assigning."""

from __future__ import annotations

from ortools.sat.python import cp_model

from cpsat_plugins import ConstraintConfig
from task_assigning import constraint_costs
from task_assigning.constraints import Constraint
from task_assigning.models import TaskAssigningSolutionModel

from .constraints import REGISTRY, TaskContext

__all__ = ["catalog", "solve"]

AFFINITY_MULTIPLIER = {"NONE": 4, "LOW": 3, "MEDIUM": 2, "HIGH": 1}


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def solve(
    problem: TaskAssigningSolutionModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: list[ConstraintConfig | Constraint | str | dict] | None = None,
    _fixed_employee_by_task: list[int] | None = None,
    _fixed_starts: list[int] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> TaskAssigningSolutionModel:
    model = cp_model.CpModel()
    objective = REGISTRY.objective(
        model,
        ("hard", "soft0", "soft1", "soft2"),
        constraints,
    )
    task_count, employee_count = len(problem.tasks), len(problem.employees)
    task_type = {item.code: item for item in problem.task_types}
    duration = {
        (e, t): task_type[task.task_type_code].base_duration
        * AFFINITY_MULTIPLIER[
            employee.customer_to_affinity.get(task.customer_id, "NONE")
        ]
        for e, employee in enumerate(problem.employees)
        for t, task in enumerate(problem.tasks)
    }
    horizon = max(
        1,
        max((task.min_start_time for task in problem.tasks), default=0)
        + sum(
            max(duration[e, t] for e in range(employee_count))
            for t in range(task_count)
        )
        if employee_count
        else 1,
    )
    assigned = {
        (e, t): model.new_bool_var(f"employee_{e}_task_{t}")
        for e in range(employee_count)
        for t in range(task_count)
    }
    unassigned = [model.new_bool_var(f"task_{t}_unassigned") for t in range(task_count)]
    start = [
        model.new_int_var(0, horizon, f"task_{t}_start") for t in range(task_count)
    ]
    end = [model.new_int_var(0, horizon, f"task_{t}_end") for t in range(task_count)]
    for t, task in enumerate(problem.tasks):
        model.add_exactly_one(
            [unassigned[t], *(assigned[e, t] for e in range(employee_count))]
        )
        model.add(start[t] >= task.min_start_time)
        model.add(
            end[t]
            == start[t]
            + sum(duration[e, t] * assigned[e, t] for e in range(employee_count))
        )
        if _fixed_employee_by_task is not None:
            if len(_fixed_employee_by_task) != task_count:
                raise ValueError(
                    "_fixed_employee_by_task must contain one value per task"
                )
            employee = _fixed_employee_by_task[t]
            if employee == -1:
                model.add(unassigned[t] == 1)
            elif 0 <= employee < employee_count:
                model.add(assigned[employee, t] == 1)
            else:
                raise ValueError("_fixed_employee_by_task contains an invalid value")
        if _fixed_starts is not None:
            if len(_fixed_starts) != task_count:
                raise ValueError("_fixed_starts must contain one value per task")
            model.add(start[t] == _fixed_starts[t])

    # There are no sequence-dependent transition costs, so optional intervals
    # are a much smaller exact formulation than one O(E*T^2) circuit per employee.
    contributions_by_employee = []
    for e in range(employee_count):
        intervals = []
        contributions = []
        for t in range(task_count):
            local_end = model.new_int_var(
                0, horizon, f"employee_{e}_task_{t}_local_end"
            )
            intervals.append(
                model.new_optional_interval_var(
                    start[t],
                    duration[e, t],
                    local_end,
                    assigned[e, t],
                    f"employee_{e}_task_{t}_interval",
                )
            )
            model.add(local_end == end[t]).only_enforce_if(assigned[e, t])
            contribution = model.new_int_var(0, horizon, f"employee_{e}_task_{t}_end")
            model.add(contribution == end[t]).only_enforce_if(assigned[e, t])
            model.add(contribution == 0).only_enforce_if(assigned[e, t].Not())
            contributions.append(contribution)
        model.add_no_overlap(intervals)
        contributions_by_employee.append(contributions)

    REGISTRY.build(
        TaskContext(
            model,
            problem,
            task_type,
            duration,
            horizon,
            assigned,
            unassigned,
            end,
            contributions_by_employee,
        ),
        objective,
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
    for e, employee in enumerate(solved.employees):
        order = sorted(
            (t for t in range(task_count) if solver.value(assigned[e, t])),
            key=lambda t: (solver.value(start[t]), t),
        )
        employee.tasks = [solved.tasks[t].id for t in order]
    for t, task in enumerate(solved.tasks):
        task.start_time = (
            None if solver.value(unassigned[t]) else solver.value(start[t])
        )
    score = objective.score_costs(constraint_costs(solved.to_core()))
    solved.score_hard = score["hard"]
    solved.score_soft0 = score["soft0"]
    solved.score_soft1 = score["soft1"]
    solved.score_soft2 = score["soft2"]
    return solved
