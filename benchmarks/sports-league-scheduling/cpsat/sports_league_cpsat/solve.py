"""CP-SAT formulation for sports league round assignment."""

from __future__ import annotations

from sports_league import calculate_score, constraint_costs
from sports_league.constraints import Constraint
from sports_league.models import LeagueScheduleModel
from cpsat_plugins import ConstraintConfig
from ortools.sat.python import cp_model

from .constraints import LeagueContext, REGISTRY

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def solve(
    problem: LeagueScheduleModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: list[ConstraintConfig | Constraint | str | dict] | None = None,
    _fixed_rounds: list[int] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> LeagueScheduleModel:
    if problem.matches and not problem.rounds:
        raise ValueError("at least one round is required when matches exist")
    model = cp_model.CpModel()
    match_count, round_count = len(problem.matches), len(problem.rounds)
    x = {
        (match, round_): model.new_bool_var(f"match_{match}_round_{round_}")
        for match in range(match_count)
        for round_ in range(round_count)
    }
    for match in range(match_count):
        model.add_exactly_one(x[match, round_] for round_ in range(round_count))
    if _fixed_rounds is not None:
        if len(_fixed_rounds) != match_count:
            raise ValueError("_fixed_rounds must contain one round per match")
        for match, round_ in enumerate(_fixed_rounds):
            model.add(x[match, round_] == 1)

    max_round_index = max((round_.index for round_ in problem.rounds), default=-1)
    round_position_by_index = {
        round_.index: index for index, round_ in enumerate(problem.rounds)
    }
    distance = problem.distance_to_team or [
        [0] * len(problem.teams) for _ in problem.teams
    ]
    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)
    REGISTRY.build(
        LeagueContext(
            model,
            problem,
            x,
            distance,
            max_round_index,
            round_position_by_index,
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
    for match, match_data in enumerate(solved.matches):
        match_data.round = next(
            round_ for round_ in range(round_count) if solver.value(x[match, round_])
        )
    core = solved.to_core()
    calculate_score(core)
    result = LeagueScheduleModel.from_core(core)
    score = objective.score_costs(constraint_costs(core))
    result.score_hard = score["hard"]
    result.score_soft = score["soft"]
    return result
