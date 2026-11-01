"""Exact CP-SAT formulation for semiconductor fab scheduling.

An interval model: per operation a start and an end, and per eligible machine an optional
interval of that option's duration; exactly one option is chosen. Every constraint is a
``ConstraintPlugin`` of the domain spec of the same name (``constraints.py``): the selected
ones post their structural rule or add their exact raw cost to a lexicographic (hard, soft)
objective. The result is rescored by the C++ engine with the same selection.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping

from cpsat_plugins import ConstraintConfig
from ortools.sat.python import cp_model
from semiconductor_scheduling import calculate_score, constraint_costs
from semiconductor_scheduling.constraints import Constraint
from semiconductor_scheduling.models import AssignmentModel, SemiconductorScheduleModel

from .constraints import REGISTRY, Choice, FabContext

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def _horizon(problem: SemiconductorScheduleModel, total_duration: int, fixed) -> int:
    total_downtime = sum(w.end - w.start for m in problem.machines for w in m.unavailable)
    total_downtime += sum(w.end - w.start for r in problem.auxiliary_resources for w in r.unavailable)
    total_setup = sum(setup.duration for setup in problem.setup_times)
    total_transport = sum(item.duration for item in problem.transport_times) * max(
        1, sum(len(lot.operations) for lot in problem.lots)
    )
    total_queue = sum(op.min_queue_time for lot in problem.lots for op in lot.operations)
    latest = max(
        [lot.release_time for lot in problem.lots]
        + [w.end for item in [*problem.machines, *problem.auxiliary_resources] for w in item.unavailable]
        + [item.end for item in problem.pinned_assignments]
        + [item.end for item in fixed or []],
        default=0,
    )
    return max(1, latest + total_duration + total_downtime + total_setup + total_transport + total_queue)


def solve(
    problem: SemiconductorScheduleModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
    _fixed_assignments: list[AssignmentModel | Mapping] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> SemiconductorScheduleModel:
    """Solve `problem` without mutating it and return a scored solution.

    ``constraints`` replaces the default selection (None = every constraint at its
    default). The test hooks: ``_fixed_assignments`` fixes every operation's machine, start
    and end (an assignment the model cannot represent — an ineligible machine or a wrong
    duration — makes it infeasible); ``_proxy_score`` receives the objective's own
    {level: -raw} totals.
    """
    model = cp_model.CpModel()
    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)

    rows = [
        (lot_index, position, lot, operation)
        for lot_index, lot in enumerate(problem.lots)
        for position, operation in enumerate(lot.operations)
    ]
    first_row, row = [], 0
    for lot in problem.lots:
        first_row.append(row)
        row += len(lot.operations)

    fixed = None
    if _fixed_assignments is not None:
        fixed = [item if isinstance(item, AssignmentModel) else AssignmentModel(**item) for item in _fixed_assignments]

    # One option per machine: the LAST eligible option on it, the one the engine scores.
    options_of = []
    for _lot_index, _position, _lot, operation in rows:
        last = {}
        for option in operation.eligible_machines:
            last[option.machine_id] = option
        options_of.append(list(last.values()))
    total_duration = sum(max(option.duration for option in options) for options in options_of)
    horizon = _horizon(problem, total_duration, fixed)

    starts, ends, choices = [], [], []
    for r, (lot_index, position, _lot, _operation) in enumerate(rows):
        start = model.new_int_var(0, horizon, f"start_{lot_index}_{position}")
        end = model.new_int_var(0, horizon, f"end_{lot_index}_{position}")
        starts.append(start)
        ends.append(end)
        row_choices = []
        for option in options_of[r]:
            present = model.new_bool_var(f"choose_{lot_index}_{position}_{option.machine_id}")
            interval = model.new_optional_interval_var(
                start, option.duration, end, present, f"run_{lot_index}_{position}_{option.machine_id}"
            )
            row_choices.append(Choice(r, option.machine_id, option.duration, option.preference_cost, present, interval))
        model.add_exactly_one(choice.present for choice in row_choices)
        choices.append(row_choices)

    if fixed is not None:
        row_of = {(lot.id, operation.id): r for r, (_l, _p, lot, operation) in enumerate(rows)}
        seen = set()
        for item in fixed:
            r = row_of[(item.lot_id, item.operation_id)]
            seen.add(r)
            choice = next((c for c in choices[r] if c.machine_id == item.machine_id), None)
            if choice is None or choice.duration != item.end - item.start:
                model.add_bool_or([])  # not representable: infeasible
                continue
            model.add(choice.present == 1)
            model.add(starts[r] == item.start)
            model.add(ends[r] == item.end)
        if len(seen) != len(rows):
            raise ValueError("_fixed_assignments must assign every operation once")

    context = FabContext(
        model=model,
        problem=problem,
        rows=rows,
        first_row=first_row,
        starts=starts,
        ends=ends,
        choices=choices,
        machine_by_id={machine.id: machine for machine in problem.machines},
        setups={(s.machine_group, s.from_family, s.to_family): s.duration for s in problem.setup_times},
        transports={(t.from_group, t.to_group): t.duration for t in problem.transport_times},
        horizon=horizon,
        total_duration=total_duration,
    )
    REGISTRY.build(context, objective)

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(f"CP-SAT did not find a solution (status={solver.status_name(status)})")
    if _proxy_score is not None:
        _proxy_score.update(objective.score(solver))

    solved = problem.model_copy(deep=True)
    solved.assignments = []
    solved.warm_start = False
    batch_ids: dict[tuple[str, str, int], str] = {}
    for r, (_lot_index, _position, lot, operation) in enumerate(rows):
        choice = next(c for c in choices[r] if solver.boolean_value(c.present))
        start = solver.value(starts[r])
        batch_id = None
        if context.machine_by_id[choice.machine_id].batch_capacity > 1:
            batch_id = batch_ids.setdefault((choice.machine_id, operation.family, start), f"B{len(batch_ids) + 1:03d}")
        solved.assignments.append(
            AssignmentModel(
                lot_id=lot.id,
                operation_id=operation.id,
                machine_id=choice.machine_id,
                start=start,
                end=solver.value(ends[r]),
                batch_id=batch_id,
            )
        )

    core = solved.to_core()
    calculate_score(core)  # makespan and the totals, from every constraint
    result = SemiconductorScheduleModel.from_core(core)
    selected = objective.score_costs(constraint_costs(core))
    result.score_hard = selected["hard"]
    result.score_soft = float(selected["soft"])
    return result
