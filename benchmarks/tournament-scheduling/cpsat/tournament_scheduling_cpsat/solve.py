"""CP-SAT load-balancing model for tournament team assignment."""

from __future__ import annotations

from ortools.sat.python import cp_model

from cpsat_plugins import ConstraintConfig
from tournament_scheduling import calculate_score, constraint_costs
from tournament_scheduling.constraints import Constraint
from tournament_scheduling.models import TournamentScheduleModel

from .constraints import REGISTRY, TournamentContext

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def solve(
    problem: TournamentScheduleModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: list[ConstraintConfig | Constraint | str | dict] | None = None,
) -> TournamentScheduleModel:
    """Optimize hard constraints, team load, then confrontation balance."""
    if problem.team_assignments and not problem.teams:
        raise ValueError("at least one team is required when assignments exist")
    model = cp_model.CpModel()
    objective = REGISTRY.objective(model, ("hard", "medium", "soft"), constraints)
    assignment_count, team_count = len(problem.team_assignments), len(problem.teams)
    day_count = len(problem.days)
    x = {
        (assignment, team): model.new_bool_var(f"assignment_{assignment}_team_{team}")
        for assignment in range(assignment_count)
        for team in range(team_count)
    }
    for a, assignment in enumerate(problem.team_assignments):
        model.add_exactly_one(x[a, team] for team in range(team_count))
        if assignment.pinned:
            if assignment.team is None or not 0 <= assignment.team < team_count:
                raise ValueError(
                    f"pinned assignment {assignment.id!r} needs a valid team"
                )
            model.add(x[a, assignment.team] == 1)

    by_day = [
        [
            a
            for a, assignment in enumerate(problem.team_assignments)
            if assignment.day == day
        ]
        for day in range(day_count)
    ]
    cell = {}
    for team in range(team_count):
        for day in range(day_count):
            members = [x[a, team] for a in by_day[day]]
            occupied = model.new_bool_var(f"team_{team}_day_{day}")
            if members:
                model.add_max_equality(occupied, members)
            else:
                model.add(occupied == 0)
            cell[team, day] = occupied

    REGISTRY.build(TournamentContext(model, problem, x, by_day, cell), objective)


    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(
            f"CP-SAT did not find a solution (status={solver.status_name(status)})"
        )

    solved = problem.model_copy(deep=True)
    for a, assignment in enumerate(solved.team_assignments):
        assignment.team = next(
            team for team in range(team_count) if solver.value(x[a, team])
        )
    core = solved.to_core()
    calculate_score(core)
    result = TournamentScheduleModel.from_core(core)
    # Sum-of-squares is a monotone search surrogate for square-root
    # unfairness; reporting always uses the canonical constraint calculation.
    score = objective.score_costs(constraint_costs(core))
    result.score_hard = score["hard"]
    result.score_soft = score["medium"] * 1_000_000 + score["soft"]
    return result
