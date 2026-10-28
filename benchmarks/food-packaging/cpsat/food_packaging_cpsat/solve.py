"""Sequence-dependent CP-SAT model for food packaging."""

from __future__ import annotations

from ortools.sat.python import cp_model

from cpsat_plugins import ConstraintConfig
from food_packaging import calculate_score, constraint_costs
from food_packaging.constraints import Constraint
from food_packaging.models import (
    PackagingScheduleModel,
    datetime_to_minute,
    minute_to_datetime,
)

from .constraints import PackagingContext, REGISTRY

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def _and(model: cp_model.CpModel, left, right, name: str):
    both = model.new_bool_var(name)
    model.add(both <= left)
    model.add(both <= right)
    model.add(both >= left + right - 1)
    return both


def solve(
    problem: PackagingScheduleModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: list[ConstraintConfig | Constraint | str | dict] | None = None,
    _fixed_line_orders: list[list[int]] | None = None,
    _fixed_operator_by_line: list[int] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> PackagingScheduleModel:
    model = cp_model.CpModel()
    objective = REGISTRY.objective(model, ("hard", "medium", "soft"), constraints)
    line_count, job_count = len(problem.lines), len(problem.jobs)
    if line_count and not problem.operators:
        raise ValueError("at least one operator is required when lines exist")
    time_origins = [datetime_to_minute(line.start_date_time) for line in problem.lines]
    time_origins.extend(datetime_to_minute(job.min_start_time) for job in problem.jobs)
    origin = min(time_origins, default=0)
    duration = [int(job.duration.total_seconds()) // 60 for job in problem.jobs]
    max_cleanup = max(
        (
            int(value.total_seconds()) // 60
            for product in problem.products
            for value in product.cleaning_durations.values()
        ),
        default=0,
    )
    horizon = max(
        1,
        max(
            (datetime_to_minute(job.max_end_time) - origin for job in problem.jobs),
            default=0,
        )
        + sum(duration)
        + job_count * max_cleanup,
    )
    product_by_id = {product.id: product for product in problem.products}
    assigned = {
        (line, job): model.new_bool_var(f"line_{line}_job_{job}")
        for line in range(line_count)
        for job in range(job_count)
    }
    unassigned = [
        model.new_bool_var(f"job_{job}_unassigned") for job in range(job_count)
    ]
    cleaning_start = [
        model.new_int_var(0, horizon, f"job_{job}_cleaning_start")
        for job in range(job_count)
    ]
    production_start = [
        model.new_int_var(0, horizon, f"job_{job}_production_start")
        for job in range(job_count)
    ]
    end = [model.new_int_var(0, horizon, f"job_{job}_end") for job in range(job_count)]
    for job in range(job_count):
        model.add_exactly_one(
            [unassigned[job], *(assigned[line, job] for line in range(line_count))]
        )
        model.add(end[job] == production_start[job] + duration[job])

    operator = {
        (line, op): model.new_bool_var(f"line_{line}_operator_{op}")
        for line in range(line_count)
        for op in range(len(problem.operators))
    }
    operator_index = {item.id: i for i, item in enumerate(problem.operators)}
    for line, line_data in enumerate(problem.lines):
        model.add_exactly_one(
            operator[line, op] for op in range(len(problem.operators))
        )
        if line_data.operator_id is not None:
            model.add(operator[line, operator_index[line_data.operator_id]] == 1)

    routes: dict[int, list[tuple[int, int, object]]] = {}
    cleaning_windows_by_operator = {op: [] for op in range(len(problem.operators))}
    line_busy = []
    for line, line_data in enumerate(problem.lines):
        line_start = datetime_to_minute(line_data.start_date_time) - origin
        empty = model.new_bool_var(f"line_{line}_empty")
        count = sum(assigned[line, job] for job in range(job_count))
        model.add(count == 0).only_enforce_if(empty)
        model.add(count >= 1).only_enforce_if(empty.Not())
        arcs: list[tuple[int, int, object]] = [(0, 0, empty)]
        incoming: dict[int, list[tuple[object, int]]] = {
            job: [] for job in range(job_count)
        }
        line_end_parts = []
        for job in range(job_count):
            node = job + 1
            arcs.append((node, node, assigned[line, job].Not()))
            first = model.new_bool_var(f"line_{line}_first_{job}")
            last = model.new_bool_var(f"line_{line}_last_{job}")
            arcs.extend(((0, node, first), (node, 0, last)))
            model.add(cleaning_start[job] == line_start).only_enforce_if(first)
            incoming[job].append((first, 0))
            last_end = model.new_int_var(0, horizon, f"line_{line}_job_{job}_last_end")
            model.add(last_end == end[job]).only_enforce_if(last)
            model.add(last_end == 0).only_enforce_if(last.Not())
            line_end_parts.append(last_end)
        for previous in range(job_count):
            previous_product = problem.jobs[previous].product_id
            for current in range(job_count):
                if previous == current:
                    continue
                arc = model.new_bool_var(f"line_{line}_arc_{previous}_{current}")
                arcs.append((previous + 1, current + 1, arc))
                cleanup_value = product_by_id[
                    problem.jobs[current].product_id
                ].cleaning_durations.get(previous_product)
                if cleanup_value is None:
                    model.add(arc == 0)
                    cleanup = 0
                else:
                    cleanup = int(cleanup_value.total_seconds()) // 60
                model.add(cleaning_start[current] == end[previous]).only_enforce_if(arc)
                incoming[current].append((arc, cleanup))
        model.add_circuit(arcs)
        routes[line] = arcs
        for job in range(job_count):
            cleanup_size = model.new_int_var(
                0, max_cleanup, f"line_{line}_job_{job}_cleanup"
            )
            model.add(
                cleanup_size == sum(value * literal for literal, value in incoming[job])
            ).only_enforce_if(assigned[line, job])
            model.add(
                production_start[job] == cleaning_start[job] + cleanup_size
            ).only_enforce_if(assigned[line, job])
            for op in range(len(problem.operators)):
                present = _and(
                    model,
                    assigned[line, job],
                    operator[line, op],
                    f"line_{line}_job_{job}_operator_{op}",
                )
                cleaning_windows_by_operator[op].append(
                    (cleaning_start[job], production_start[job], present)
                )
        line_end = model.new_int_var(0, horizon, f"line_{line}_end")
        model.add_max_equality(line_end, line_end_parts or [0])
        busy = model.new_int_var(0, horizon, f"line_{line}_busy")
        model.add_max_equality(busy, [0, line_end - line_start])
        line_busy.append(busy)

    if _fixed_line_orders is not None:
        if len(_fixed_line_orders) != line_count:
            raise ValueError("_fixed_line_orders must contain one order per line")
        flattened = [job for order in _fixed_line_orders for job in order]
        if len(flattened) != len(set(flattened)) or any(
            job < 0 or job >= job_count for job in flattened
        ):
            raise ValueError("_fixed_line_orders contains an invalid or duplicate job")
        selected_jobs = set(flattened)
        for job in range(job_count):
            model.add(unassigned[job] == (job not in selected_jobs))
        for line, order in enumerate(_fixed_line_orders):
            selected = set(order)
            expected = {
                (job + 1, job + 1) for job in range(job_count) if job not in selected
            }
            expected.update(
                {(0, 0)}
                if not order
                else {
                    (0, order[0] + 1),
                    *((left + 1, right + 1) for left, right in zip(order, order[1:])),
                    (order[-1] + 1, 0),
                }
            )
            for tail, head, literal in routes[line]:
                model.add(literal == ((tail, head) in expected))

    if _fixed_operator_by_line is not None:
        if len(_fixed_operator_by_line) != line_count:
            raise ValueError(
                "_fixed_operator_by_line must contain one operator per line"
            )
        for line, selected in enumerate(_fixed_operator_by_line):
            if selected < 0 or selected >= len(problem.operators):
                raise ValueError("_fixed_operator_by_line contains an invalid operator")
            for op in range(len(problem.operators)):
                model.add(operator[line, op] == (op == selected))

    REGISTRY.build(
        PackagingContext(
            model,
            problem,
            origin,
            horizon,
            duration,
            unassigned,
            end,
            line_busy,
            cleaning_windows_by_operator,
        ),
        objective,
    )

    # Preserve the placement of pinned jobs; pinned jobs absent from every
    # input line remain absent.
    original_line = {
        job_id: line
        for line, line_data in enumerate(problem.lines)
        for job_id in line_data.job_ids
    }
    for job, job_data in enumerate(problem.jobs):
        if not job_data.pinned:
            continue
        if job_data.id in original_line:
            model.add(assigned[original_line[job_data.id], job] == 1)
        else:
            model.add(unassigned[job] == 1)

    solver = cp_model.CpSolver()
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    solver.parameters.max_time_in_seconds = time_limit_seconds
    status = objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(
            f"CP-SAT did not find a solution (status={solver.status_name(status)})"
        )
    if _proxy_score is not None:
        _proxy_score.update(objective.score(solver))

    solved = problem.model_copy(deep=True)
    for line, line_data in enumerate(solved.lines):
        successors = {
            tail - 1: head - 1
            for tail, head, literal in routes[line]
            if tail != head and solver.value(literal)
        }
        order = []
        current = successors.get(-1, -1)
        while current >= 0:
            order.append(current)
            current = successors.get(current, -1)
        line_data.job_ids = [solved.jobs[job].id for job in order]
        line_data.operator_id = solved.operators[
            next(
                op
                for op in range(len(solved.operators))
                if solver.value(operator[line, op])
            )
        ].id
        for job in order:
            solved.jobs[job].line_id = line_data.id
            solved.jobs[job].start_cleaning_date_time = minute_to_datetime(
                origin + solver.value(cleaning_start[job])
            )
            solved.jobs[job].start_production_date_time = minute_to_datetime(
                origin + solver.value(production_start[job])
            )
            solved.jobs[job].end_date_time = minute_to_datetime(
                origin + solver.value(end[job])
            )
    # Score the chosen solution with the canonical raw constraint costs,
    # including for an explicit subset.
    core = solved.to_core()
    calculate_score(core)
    score = objective.score_costs(constraint_costs(core))
    solved.score_hard = score["hard"]
    solved.score_medium = score["medium"]
    solved.score_soft = score["soft"]
    return solved
