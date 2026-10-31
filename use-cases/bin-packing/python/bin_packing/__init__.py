"""Bin packing with shared exact/metaheuristic constraint semantics."""

from ._core import (
    BinPackingProblem,
    Item,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    list_constraints,
)
from .models import with_initial_solution

__all__ = [
    "BinPackingProblem",
    "Item",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "constraint_costs",
    "list_constraints",
    "solve",
    "with_initial_solution",
]


def solve(
    problem: BinPackingProblem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    config: SolverConfig | None = None,
    constraints=None,
) -> Score:
    cfg = config if config is not None else SolverConfig()
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    solver = Solver(cfg)
    return solver.solve(problem) if constraints is None else solver.solve_with_constraints(problem, constraints)
