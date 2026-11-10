"""Bin packing — Python wrapper of the C++ solver on the optimaru engine.

Uniform-capacity one-dimensional bin packing with optional item colours and
conflict pairs: one assignment-shape decision variable (`Item.bin`, a 0-based bin index,
-1 = unassigned) puts every item in one of n identical bins.

Score, at the default weights (1 each):
  hard = -(load above capacity, per bin          # bin_capacity_cost
           + items beyond the first of their colour, per bin   # color_conflict_cost
           + conflict pairs sharing a bin)        # item_conflict_cost
  soft = -(bins in use)                           # used_bins_cost
The problem carries no weights: change one through a constraint selection, e.g.
``solve(problem, constraints=[..., UsedBinsCost(weight=2)])``
(see `optimaru.bin_packing.constraints`). `constraint_info()` lists every constraint with its
default level and weight.

Basic usage::

    import optimaru.bin_packing as bp

    problem = bp.BinPackingProblem(10, [bp.Item("A", 8), bp.Item("B", 7), bp.Item("C", 3), bp.Item("D", 2)])
    bp.solve(problem, time_limit_seconds=1.0, random_seed=42)
    print(problem.score, problem.bins_used(), problem.bin_loads())

`problem.items` returns copies: write bins with ``problem.set_assignment([...])``.
The JSON contract is `optimaru.bin_packing.models` (pydantic).
"""

from typing import TypeVar

from .._solve import solve_in_place, solver_config
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
from .models import BinPackingProblemModel, with_initial_solution

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


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


def solve(
    problem: _Problem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> _Problem:
    """High-level API that solves `problem` in place and returns it.

    `problem` is a `BinPackingProblem` (solved in place and returned) or a
    `models.BinPackingProblemModel` (converted, solved, and returned as a new model; the
    one passed in is left as it was).

    The score is on the result: ``solve(problem).score`` (``.hard``, ``.soft``).

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS
    on). Explicit keyword arguments override config. ``spent_limit_seconds = 0`` on
    the config skips the search: the construction (or the seed of a warm start,
    completed where an item is still -1) comes back as it is.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached;
    ``on_new_best(step, score)`` is called on every new best solution.

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly (see `optimaru.bin_packing.constraints`).
    Only the listed constraints are scored, a name picked several times once per pick;
    None uses all four default constraints at their default weights. With constraints
    the solve is single-start
    (``parallel_solver_count`` is ignored)::

        from optimaru.bin_packing.constraints import get_constraints
        bp.solve(problem, constraints=get_constraints())
    """
    cfg = solver_config(
        SolverConfig,
        config,
        time_limit_seconds=time_limit_seconds,
        random_seed=random_seed,
        parallel_solver_count=parallel_solver_count,
        stop_at_first_feasible=stop_at_first_feasible,
        full_assert=full_assert,
        on_new_best=on_new_best,
    )
    if isinstance(problem, BinPackingProblem):
        solve_in_place(Solver(cfg), problem, constraints)
        return problem
    if isinstance(problem, BinPackingProblemModel):
        core = problem.to_core()
        solve_in_place(Solver(cfg), core, constraints)
        return BinPackingProblemModel.from_core(core)
    raise TypeError(f"problem must be a BinPackingProblem or a models.BinPackingProblemModel: {type(problem)!r}")
