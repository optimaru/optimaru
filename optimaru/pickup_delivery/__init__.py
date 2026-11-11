"""Pickup and delivery with time windows (PDPTW) — C++ core, Python bindings.

A transport **request** is a pickup and a dropoff that one vehicle must serve,
pickup first, without ever exceeding its capacity in between. That coupling is
the whole difference from `optimaru.vehicle_routing`: a plain visit can be moved anywhere,
a PDP stop drags its partner along.

The solution is `PdpRoutePlan`: each vehicle's ordered `node_indices` (its
route) is the decision, and a stop in no route is unassigned (it lands
in `plan.unassigned_node_indices`).

Score (hard / medium / soft, lexicographic), at the default weights:

* **hard** — capacity peak along the route (`vehicle_capacity_cost`), time-window
  lateness (`max_end_time_cost`), and the coupling rules (`paired_request_cost`,
  `same_vehicle_cost`, `pickup_before_dropoff_cost`: a request is routed whole or
  not at all, on one vehicle, pickup before dropoff). 0 = feasible.
* **medium** — service minutes of the requests left unrouted
  (`unassigned_requests_cost`); the engine folds it into soft as ``medium * 10^9``.
* **soft** — total driving seconds (`travel_time_cost`).

`constraint_info()` lists every constraint with its default level and weight;
`optimaru.pickup_delivery.constraints` picks and re-weights them.

The solver is the shared engine in `optimaru/cpp/optimaru/`, with operators
registered on top of it whose unit is the request, not the stop. The request
moves take requests out of their routes and put each back, pickup and dropoff
together, at its cheapest feasible slot pair (`request_ruin_recreate`), or carry
a bundle of requests from one route to another and swap bundles between routes
(`bundle_moves`). A pair move repairs requests whose pair is broken
(`pair_moves`) and a pair ruin kicks whole requests, never half of one
(`pair_ruin`). All are on by default; the switches turn them off to measure
what they are worth.

Requests that share a `group` (`Delivery.group_index`, `DeliveryModel.group`) are
one job: they ride one vehicle together or all stay unassigned
(`request_group_cost`), and the request moves carry them as one unit.
"""

from .._solve import solve_in_place, solver_config
from ._core import (
    AcceptorType,
    ConstraintInfo,
    Delivery,
    Location,
    Node,
    OperatorSelectionType,
    PdpRoutePlan,
    Score,
    Solver,
    SolverConfig,
    Vehicle,
    build_random_instance,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
)
from .constraints import get_constraints
from .models import solve_request, with_initial_solution

__all__ = [
    "AcceptorType",
    "ConstraintInfo",
    "Delivery",
    "Location",
    "Node",
    "OperatorSelectionType",
    "PdpRoutePlan",
    "Score",
    "Solver",
    "SolverConfig",
    "Vehicle",
    "build_random_instance",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "solve_request",
    "with_initial_solution",
]


def solve(
    plan: PdpRoutePlan,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> PdpRoutePlan:
    """High-level API that solves `plan` in place and returns it.

    ``config`` becomes the base settings (`SolverConfig`; None = engine defaults,
    a 30 s budget). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached, ``full_assert``
    re-verifies every incremental score against a full recompute (a debugging aid,
    much slower), and ``on_new_best(step, score)`` is called on every new best.

    ``constraints`` picks the cost function directly (see
    `optimaru.pickup_delivery.constraints`). Only the listed constraints are scored;
    None uses the default constraints at their default weights::

        from optimaru.pickup_delivery.constraints import get_constraints
        pdp.solve(plan, constraints=get_constraints())

    A plan with request groups (two requests or more share a `Delivery.group_index`) is
    scored with the default constraints even when ``constraints`` is None:
    `request_group_cost` is one of them, and it is not among the seven core rules a plan is
    otherwise scored on. That is the whole default set, so the vehicle limits, pinned stops
    and required vehicles of such a plan are scored too. The engine does this itself, so
    `Solver(config).solve(plan)` and `calculate_score(plan)` read such a plan the same way.
    A list passed as ``constraints`` is used as it is. ``parallel_solver_count`` applies on
    every path: with constraints, each start scores on its own copy of them.

    The score is on the result: ``plan.score``. `solve_request` is the same solve for
    a `models` request (JSON in, JSON out).
    """
    if not isinstance(plan, PdpRoutePlan):
        raise TypeError(
            f"plan must be an instance of PdpRoutePlan: {type(plan)!r} (models.solve_request solves a models.PdpRequest)"
        )
    cfg = solver_config(
        SolverConfig,
        config,
        time_limit_seconds=time_limit_seconds,
        random_seed=random_seed,
        parallel_solver_count=parallel_solver_count,
        stop_at_first_feasible=stop_at_first_feasible,
        full_assert=full_assert,
        on_new_best=on_new_best,
        # Hard 0: -(2**63) on medium/soft is "no floor" (the engine compares hard only).
        first_feasible_limit=(0, -(2**63), -(2**63)),
    )
    solve_in_place(Solver(cfg), plan, constraints)
    return plan
