"""Bin packing — Python wrapper of the C++ solver on the optimaru engine.

Uniform-capacity one-dimensional bin packing with optional item colours and
conflict pairs: one BASIC planning variable (`Item.bin`, a 0-based bin index,
-1 = unassigned) puts every item in one of n identical bins.

Score (HardSoftScore), at the default weights (1 each):
  hard = -(load above capacity, per bin          # bin_capacity_cost
           + items beyond the first of their colour, per bin   # color_conflict_cost
           + conflict pairs sharing a bin)        # item_conflict_cost
  soft = -(bins in use)                           # used_bins_cost
The problem carries no weights: change one through a constraint selection, e.g.
``solve(problem, constraints=get_constraints([..., UsedBinsCost(weight=2)]))``
(see `bin_packing.constraints`). `constraint_info()` lists every constraint with its
default level and weight.

Basic usage::

    import bin_packing as bp

    problem = bp.BinPackingProblem(10, [bp.Item("A", 8), bp.Item("B", 7), bp.Item("C", 3), bp.Item("D", 2)])
    bp.solve(problem, time_limit_seconds=1.0, random_seed=42)
    print(problem.score, problem.bins_used(), problem.bin_loads())

`problem.items` returns copies: write bins with ``problem.set_assignment([...])``.
The JSON contract is `bin_packing.models` (pydantic).
"""

import copy

from ._core import (
    BinPackingProblem,
    ConstraintInfo,
    Item,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
)
from .models import with_initial_solution

__all__ = [
    "BinPackingProblem",
    "ConstraintInfo",
    "Item",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
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
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> Score:
    """High-level API that solves `problem` in place and returns the final `Score`.

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS
    on). Explicit keyword arguments override config. ``spent_limit_seconds = 0`` on
    the config skips the search: the construction (or the seed of a warm start,
    completed where an item is still -1) comes back as it is.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached;
    ``on_new_best(step, score)`` is called on every new best solution.

    ``constraints`` picks the cost function directly (see `bin_packing.constraints`).
    Only the listed constraints are scored; None uses all four default constraints at
    their default weights. With constraints the solve is single-start
    (``parallel_solver_count`` is ignored)::

        from bin_packing.constraints import get_constraints
        bp.solve(problem, constraints=get_constraints())
    """
    # A copy: the keyword overrides below must not leak into the caller's config.
    cfg = copy.copy(config) if config is not None else SolverConfig()
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    if stop_at_first_feasible:
        cfg.best_score_limit = (0, float("-inf"))
    if on_new_best is not None:
        cfg.on_new_best = on_new_best

    if constraints is None:
        Solver(cfg).solve(problem)
    else:
        Solver(cfg).solve_with_constraints(problem, constraints)
    return problem.score
