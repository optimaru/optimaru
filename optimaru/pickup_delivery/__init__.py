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

The solver is the shared engine in `optimaru/cpp/optimaru/`, with two PDP-specific extensions
registered on top of it: a pair-aware move operator (relocates a whole request
and repairs broken pairs) and a pair-aware ruin operator (kicks whole requests,
never half of one). Both are on by default; `pair_moves` / `pair_ruin` turn
them off to measure what they are worth.
"""

import copy

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
    "solve",
    "list_constraints",
    "constraint_costs",
    "constraint_info",
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

    The score is on the result: ``plan.score``. `solve_request` is the same solve for
    a `models` request (JSON in, JSON out).
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
        # Hard 0: -(2**63) on medium/soft is "no floor" (the engine compares hard only).
        cfg.best_score_limit = (0, -(2**63), -(2**63))
    if full_assert:
        cfg.full_assert = True
    if on_new_best is not None:
        cfg.on_new_best = on_new_best
    if constraints is None:
        Solver(cfg).solve(plan)
    else:
        Solver(cfg).solve_with_constraints(plan, constraints)
    return plan

