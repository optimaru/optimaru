"""Order picking — C++ core (C++ port of the order-picking example model) with Python helpers.

`Side`, `WarehouseLocation`, `Order`, `PickTask`, `Trolley`,
`OrderPickingSolution`, `Score`, `ConstraintInfo`, `Solver`, `SolverConfig`
and the module functions come from the compiled C++ core (`_core`). The
solution is the planning solution: each trolley's ordered `pick_task_indices`
list is the planning list variable; `trolley_index` / `previous_pick_task` /
`next_pick_task` on a pick task are the shadow variables derived from it.

Score convention (HardSoft, mirroring the Java quickstart 1:1), at the default
weights:
  hard = -(missing trolley buckets)                     # trolley_capacity_cost
  soft = -(travel distance in meters                    # distance_from_previous_pick_task_cost
                                                        #   + distance_from_last_pick_task_to_path_origin_cost
           + 1000 x per-order distinct-trolley count)   # order_split_by_trolley_cost
hard 0 == feasible. `constraint_costs(solution)` gives every constraint's raw
per-trolley cost (the only per-constraint surface), `constraint_info()` every
constraint with its default level and weight; change one through a constraint
selection (see `order_picking.constraints`).

Quick start::

    import order_picking as op

    solution = op.build_demo_data()          # the Java demo dataset
    solution = op.solve(solution, time_limit_seconds=5.0, random_seed=0)
    print(solution.score, solution.routes())

pydantic request/response models (JSON in/out) live in
`order_picking.models` (optional dependency).
"""

import copy

from ._core import (
    ConstraintInfo,
    Order,
    OrderPickingSolution,
    PickTask,
    Score,
    Side,
    Solver,
    SolverConfig,
    Trolley,
    WarehouseLocation,
    build_demo_data,
    calculate_distance,
    calculate_score,
    constraint_costs,
    constraint_info,
    distance_to_travel,
    list_constraints,
    update_shadow_variables,
)

__all__ = [
    "ConstraintInfo",
    "Order",
    "OrderPickingSolution",
    "PickTask",
    "Score",
    "Side",
    "Solver",
    "SolverConfig",
    "Trolley",
    "WarehouseLocation",
    "build_demo_data",
    "calculate_distance",
    "calculate_score",
    "distance_to_travel",
    "solve",
    "update_shadow_variables",
    "with_initial_solution",
    "list_constraints",
    "constraint_costs",
    "constraint_info",
]


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    See `models.with_initial_solution`.
    """
    # Lazy import: pydantic is an optional dependency.
    from . import models

    return models.with_initial_solution(problem, solution)


def solve(
    solution: OrderPickingSolution,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> OrderPickingSolution:
    """Solve an `OrderPickingSolution` in place and return it.

    ``config`` (a `SolverConfig`) is the base configuration; the explicit
    keyword arguments override it. ``stop_at_first_feasible`` terminates as
    soon as hard 0 is reached (best_score_limit=(0, -1e18)). The default time
    budget is the engine's 30s.

    ``constraints`` picks the cost function (see `order_picking.constraints`).
    Only the constraints in the list are scored; None keeps all four
    defaults::

        from order_picking.constraints import get_constraints
        op.solve(solution, constraints=get_constraints())
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
        cfg.best_score_limit = (0, -1e18)
    if on_new_best is not None:
        cfg.on_new_best = on_new_best
    if constraints is None:
        Solver(cfg).solve(solution)
    else:
        Solver(cfg).solve_with_constraints(solution, constraints)
    return solution
