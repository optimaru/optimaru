"""CP-SAT assignment and itinerary model for flight crews."""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from ortools.sat.python import cp_model

from flight_crew import calculate_score, constraint_costs
from flight_crew.constraints import Constraint
from flight_crew.models import FlightCrewScheduleModel
from cpsat_plugins import ConstraintConfig

from .constraints import FlightCrewContext, REGISTRY

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def solve(
    problem: FlightCrewScheduleModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
    _fixed_employees: list[int] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> FlightCrewScheduleModel:
    """Assign every duty and optimize hard violations before itinerary ends."""
    if problem.flight_assignments and not problem.employees:
        raise ValueError("at least one employee is required when assignments exist")
    model = cp_model.CpModel()
    assignment_count = len(problem.flight_assignments)
    employee_count = len(problem.employees)
    flight_by_number = {flight.flight_number: flight for flight in problem.flights}
    flights = [
        flight_by_number[assignment.flight] for assignment in problem.flight_assignments
    ]
    x = {
        (a, e): model.new_bool_var(f"assignment_{a}_employee_{e}")
        for a in range(assignment_count)
        for e in range(employee_count)
    }
    for a, assignment in enumerate(problem.flight_assignments):
        model.add_exactly_one(x[a, e] for e in range(employee_count))
        if _fixed_employees is not None:
            if len(_fixed_employees) != assignment_count:
                raise ValueError("_fixed_employees must contain one value per duty")
            employee = _fixed_employees[a]
            if not 0 <= employee < employee_count:
                raise ValueError("_fixed_employees contains an invalid employee")
            model.add(x[a, employee] == 1)
        if assignment.pinned:
            if assignment.employee is None:
                raise ValueError(
                    f"pinned assignment {assignment.id!r} needs an employee"
                )
            try:
                employee = next(
                    i
                    for i, item in enumerate(problem.employees)
                    if item.id == assignment.employee
                )
            except StopIteration as exc:
                raise ValueError(
                    f"pinned assignment {assignment.id!r} has an unknown employee"
                ) from exc
            model.add(x[a, employee] == 1)

    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)

    # For each employee, selected duties form one chronological path. Arc
    # costs express consecutive transfers and the first/last home-airport
    # penalties without introducing a time-index horizon.
    order = sorted(
        range(assignment_count), key=lambda a: (flights[a].departure_utc_date_time, a)
    )
    position = {assignment: i for i, assignment in enumerate(order)}
    first_arcs = {}
    last_arcs = {}
    transitions = {}
    for e in range(employee_count):
        empty = model.new_bool_var(f"employee_{e}_empty")
        selected_count = sum(x[a, e] for a in range(assignment_count))
        model.add(selected_count == 0).only_enforce_if(empty)
        model.add(selected_count >= 1).only_enforce_if(empty.Not())
        arcs: list[tuple[int, int, object]] = [(0, 0, empty)]
        for a in range(assignment_count):
            node = a + 1
            arcs.append((node, node, x[a, e].Not()))
            first_literal = model.new_bool_var(f"first_{a}_{e}")
            last_literal = model.new_bool_var(f"last_{a}_{e}")
            first_arcs[a, e] = first_literal
            last_arcs[a, e] = last_literal
            arcs.extend([(0, node, first_literal), (node, 0, last_literal)])
        for a in range(assignment_count):
            for b in range(assignment_count):
                if position[a] >= position[b]:
                    continue
                arc = model.new_bool_var(f"next_{a}_{b}_{e}")
                arcs.append((a + 1, b + 1, arc))
                transitions[a, b, e] = arc
        model.add_circuit(arcs)

    REGISTRY.build(
        FlightCrewContext(
            model, problem, flights, x, first_arcs, last_arcs, transitions
        ),
        objective,
    )


    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(
            f"CP-SAT did not find a solution (status={solver.status_name(status)})"
        )
    if _proxy_score is not None:
        _proxy_score.update(objective.score(solver))

    solved = problem.model_copy(deep=True)
    for a, assignment in enumerate(solved.flight_assignments):
        employee = next(e for e in range(employee_count) if solver.value(x[a, e]))
        assignment.employee = solved.employees[employee].id
    core = solved.to_core()
    calculate_score(core)
    result = FlightCrewScheduleModel.from_core(core)
    selected_score = objective.score_costs(constraint_costs(core))
    result.score_hard = selected_score["hard"]
    result.score_soft = float(selected_score["soft"])
    return result
