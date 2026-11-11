"""Order picking — C++ core with Python helpers.

`Side`, `WarehouseLocation`, `Order`, `PickTask`, `Trolley`,
`OrderPickingSolution`, `Score`, `ConstraintInfo`, `Solver`, `SolverConfig`
and the module functions come from the compiled C++ core (`_core`). Each
trolley's ordered `pick_task_indices` list is the decision; `trolley_index` /
`previous_pick_task` / `next_pick_task` on a pick task are derived from the
lists.

Score convention (hard / soft), at the default weights:
  hard = -(missing trolley buckets)                     # trolley_capacity_cost
  soft = -(travel distance in meters                    # distance_from_previous_pick_task_cost
                                                        #   + distance_from_last_pick_task_to_path_origin_cost
           + 1000 x per-order distinct-trolley count)   # order_split_by_trolley_cost
hard 0 == feasible. `constraint_costs(solution)` gives every constraint's raw
per-trolley cost (the only per-constraint surface), `constraint_info()` every
constraint with its default level and weight; change one through a constraint
selection (see `optimaru.order_picking.constraints`).

Quick start::

    import optimaru.order_picking as op

    solution = op.build_demo_data()          # the demo warehouse
    solution = op.solve(solution, time_limit_seconds=5.0, random_seed=0)
    print(solution.score, solution.routes())

pydantic request/response models (JSON in/out) live in
`optimaru.order_picking.models`; `solve` takes a `models.OrderPickingRequest` too
and returns a `models.OrderPickingResponse`.
"""

from typing import overload

from .._solve import request_with, solve_in_place, solver_config
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
from .models import (
    OrderPickingInput,
    OrderPickingRequest,
    OrderPickingResponse,
    _solve_model,
    _solve_request,
    with_initial_solution,
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
    "constraint_costs",
    "constraint_info",
    "distance_to_travel",
    "list_constraints",
    "solve",
    "update_shadow_variables",
    "with_initial_solution",
]


@overload
def solve(
    solution: OrderPickingSolution,
    *,
    time_limit_seconds: float | None = ...,
    random_seed: int | None = ...,
    parallel_solver_count: int | None = ...,
    stop_at_first_feasible: bool = ...,
    full_assert: bool = ...,
    on_new_best=...,
    config: SolverConfig | None = ...,
    constraints=...,
) -> OrderPickingSolution: ...
@overload
def solve(
    solution: OrderPickingRequest,
    *,
    time_limit_seconds: float | None = ...,
    random_seed: int | None = ...,
    parallel_solver_count: int | None = ...,
    stop_at_first_feasible: bool = ...,
    full_assert: bool = ...,
    on_new_best=...,
    config: SolverConfig | None = ...,
    constraints=...,
) -> OrderPickingResponse: ...
@overload
def solve(
    solution: OrderPickingInput,
    *,
    time_limit_seconds: float | None = ...,
    random_seed: int | None = ...,
    parallel_solver_count: int | None = ...,
    stop_at_first_feasible: bool = ...,
    full_assert: bool = ...,
    on_new_best=...,
    config: SolverConfig | None = ...,
    constraints=...,
) -> OrderPickingInput: ...
def solve(
    solution: OrderPickingSolution | OrderPickingInput | OrderPickingRequest,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> OrderPickingSolution | OrderPickingInput | OrderPickingResponse:
    """Solve an order picking problem and return it in the same shape.

    ``solution`` is an `OrderPickingSolution` (solved in place and returned) or a
    `models.OrderPickingInput`, the pydantic problem (converted to the engine shape,
    solved, and returned as a new `OrderPickingInput` with the result fields filled in,
    the one passed in left as it was).

    A `models.OrderPickingRequest`, the earlier pydantic shape that holds the problem
    with its time limit and seed, is still taken: `solve` leaves it unchanged and
    returns a `models.OrderPickingResponse`. The request carries its own
    ``timelimit``, ``random_seed``, ``num_workers`` and ``stop_at_first_feasible``, and
    they always apply: with a request, ``config.spent_limit_seconds``, ``random_seed``
    and ``parallel_solver_count`` are replaced by the request's fields, so change those
    three through the keywords. A keyword argument that is given replaces what the
    request says, and ``stop_at_first_feasible`` turns the request's flag on.

    ``config`` (a `SolverConfig`) is the base configuration; the explicit
    keyword arguments override it. ``stop_at_first_feasible`` terminates as
    soon as hard 0 is reached (best_score_limit=(0, -inf)). The default time
    budget is the engine's 30s.

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function (see `optimaru.order_picking.constraints`).
    Only the constraints in the list are scored, a name picked several times
    once per pick; None keeps all four defaults::

        from optimaru.order_picking.constraints import get_constraints
        op.solve(solution, constraints=get_constraints())
    """
    if isinstance(solution, OrderPickingRequest):
        request = request_with(
            solution,
            time_limit_seconds=time_limit_seconds,
            random_seed=random_seed,
            parallel_solver_count=parallel_solver_count,
            stop_at_first_feasible=stop_at_first_feasible,
        )
        return _solve_request(
            request, constraints=constraints, config=config, full_assert=full_assert, on_new_best=on_new_best
        )
    if not isinstance(solution, (OrderPickingSolution, OrderPickingInput)):
        raise TypeError(f"solution must be an OrderPickingSolution or a models.OrderPickingInput: {type(solution)!r}")
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
    if isinstance(solution, OrderPickingInput):
        return _solve_model(solution, cfg, constraints)
    solve_in_place(Solver(cfg), solution, constraints)
    return solution
