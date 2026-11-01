"""Entry point: problem input -> CP-SAT model -> timetabling result."""

from collections.abc import Sequence

from ortools.sat.python import cp_model

from teacher_scheduling.domain.problem import TimetableProblem
from teacher_scheduling.domain.solution import ScheduledLesson, SolveStatus, Timetable
from teacher_scheduling.solver.config import SolverConfig
from teacher_scheduling.solver.objective import PenaltyTerms
from teacher_scheduling.solver.registry import (
    Constraint,
    build_all,
    default_constraints,
    instance_keys,
    objective_term,
    weight_of,
)
from teacher_scheduling.solver.rooms import assign_rooms
from teacher_scheduling.solver.variables import TimetableVariables

_STATUS_BY_CPSAT: dict[int, SolveStatus] = {
    cp_model.OPTIMAL: SolveStatus.OPTIMAL,
    cp_model.FEASIBLE: SolveStatus.FEASIBLE,
    cp_model.INFEASIBLE: SolveStatus.INFEASIBLE,
    cp_model.MODEL_INVALID: SolveStatus.MODEL_INVALID,
    cp_model.UNKNOWN: SolveStatus.UNKNOWN,
}


def solve(
    problem: TimetableProblem,
    config: SolverConfig | None = None,
    *,
    constraints: Sequence[Constraint] | None = None,
) -> Timetable:
    """Builds the model (variables -> constraints), solves it with CP-SAT and converts the result to a Timetable.

    With constraints None, assembles the default configuration (every engine
    constraint at its default level) with the `config.weights` soft weights. A
    given constraint list is the whole list — to keep the defaults use
    `registry.merge(default_constraints(), ...)`; a built-in constraint's
    ``weight=None`` is the engine default, not `config.weights`. A constraint
    listed several times adds one objective term per entry, each with its own
    knobs (an offset shaped as the engine shapes it); `Timetable.penalties`
    reports each name's raw count once.
    Solutions obtained with relaxed or injected custom constraints are outside the
    scope of `validate.validate_timetable` (see the `teacher_scheduling.solver.registry` docstring).
    """
    config = config or SolverConfig()

    model = cp_model.CpModel()
    variables = TimetableVariables(model, problem)
    if constraints is None:
        constraints = default_constraints(config.weights)
    constraints = list(constraints)
    penalties = build_all(model, variables, constraints)
    _minimize(model, variables, penalties, constraints, config)

    solver = _make_solver(config)
    status = _STATUS_BY_CPSAT.get(solver.solve(model), SolveStatus.UNKNOWN)
    if not status.has_solution:
        return Timetable(status=status, wall_time_seconds=solver.wall_time)

    lessons = assign_rooms(problem, _extract_lessons(solver, variables))
    return Timetable(
        status=status,
        lessons=tuple(lessons),
        objective_value=solver.objective_value if model.has_objective() else None,
        best_objective_bound=solver.best_objective_bound if model.has_objective() else None,
        wall_time_seconds=solver.wall_time,
        penalties=_penalty_values(solver, penalties),
    )


def _minimize(
    model: cp_model.CpModel,
    variables: TimetableVariables,
    penalties: PenaltyTerms,
    constraints: Sequence[Constraint],
    config: SolverConfig,
) -> None:
    """Minimises the weighted sum of the (offset-shaped) penalty terms, one per entry. Terms with
    weight 0 leave the objective.

    A penalty expression stays in `penalties` even with weight 0 — removing it
    from the objective is not the same as hiding it from the result.
    """
    objective = []
    for key, constraint in zip(instance_keys(constraints), constraints, strict=True):
        expr = penalties.get(key)
        weight = weight_of(constraint, config.weights)
        if expr is None or not weight:
            continue
        term = objective_term(constraint, model, variables, expr)
        objective.append(weight * term)
    if objective:
        model.minimize(sum(objective))


def _make_solver(config: SolverConfig) -> cp_model.CpSolver:
    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = config.time_limit_seconds
    solver.parameters.num_workers = config.num_workers
    solver.parameters.random_seed = config.random_seed
    solver.parameters.log_search_progress = config.log_search_progress
    if config.relative_gap_limit is not None:
        solver.parameters.relative_gap_limit = config.relative_gap_limit
    return solver


def _extract_lessons(solver: cp_model.CpSolver, variables: TimetableVariables) -> list[ScheduledLesson]:
    """Moves the x variables that are on into timetable cells. Special rooms come later."""
    lessons = []
    for course in variables.problem.courses:
        for slot in variables.allowed_slots[course.id]:
            if solver.value(variables.x[course.id, slot]):
                lessons.append(
                    ScheduledLesson(
                        course_id=course.id,
                        class_group_id=course.class_group_id,
                        subject_id=course.subject_id,
                        teacher_ids=course.teacher_ids,
                        slot=slot,
                    )
                )
    return lessons


def _penalty_values(solver: cp_model.CpSolver, penalties: PenaltyTerms) -> dict[str, int]:
    """The raw count of each penalized name, once: a repeated entry ("<name>#<k>") counts the same."""
    values: dict[str, int] = {}
    for key, expr in penalties.items():
        values.setdefault(key.partition("#")[0], int(solver.value(expr)))
    return values
