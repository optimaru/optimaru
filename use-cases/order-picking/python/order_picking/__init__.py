"""Order picking — C++ core (Timefold order-picking quickstart port) with Python helpers.

`Side`, `WarehouseLocation`, `Order`, `PickTask`, `Trolley`,
`OrderPickingSolution`, `Score`, `ConstraintTotals`, `Solver`, `SolverConfig`
and the module functions come from the compiled C++ core (`_core`). The
solution is the planning solution: each trolley's ordered `pick_task_indices`
list is the planning list variable; `trolley_index` / `previous_pick_task` /
`next_pick_task` on a pick task are the shadow variables derived from it.

Score convention (HardSoft, mirroring the Java quickstart 1:1):
hard = -(missing trolley buckets), soft = -(travel distance in meters +
1000 * per-order distinct-trolley count). hard 0 == feasible.

Quick start::

    import order_picking as op

    solution = op.build_demo_data()          # the Java demo dataset
    solution = op.solve(solution, time_limit_seconds=5.0, random_seed=0)
    print(solution.score, solution.routes())

pydantic request/response models (JSON in/out) live in
`order_picking.models` (optional dependency).
"""

from ._core import (
    ConstraintTotals,
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
    calculate_totals,
    constraint_costs,
    distance_to_travel,
    list_constraints,
    update_shadow_variables,
)

__all__ = [
    "ConstraintTotals",
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
    "calculate_totals",
    "distance_to_travel",
    "solve",
    "update_shadow_variables",
    "list_constraints",
    "constraint_costs",
]


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
    cfg = config if config is not None else SolverConfig()
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
