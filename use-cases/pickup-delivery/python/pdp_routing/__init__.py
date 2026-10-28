"""Pickup and delivery with time windows (PDPTW) — C++ core, Python bindings.

A transport **request** is a pickup and a dropoff that one vehicle must serve,
pickup first, without ever exceeding its capacity in between. That coupling is
the whole difference from `vrp_routing`: a plain visit can be moved anywhere,
a PDP stop drags its partner along.

The planning solution is `PdpRoutePlan`: each vehicle's ordered `node_indices`
is the planning list variable, and a stop in no route is unassigned (it lands
in `plan.unassigned_node_indices`).

Score (HardMediumSoft, lexicographic):

* **hard** — capacity peak along the route, time-window lateness, and the
  coupling rules (a request is routed whole or not at all, on one vehicle,
  pickup before dropoff). 0 = feasible.
* **medium** — service minutes of the requests left unrouted.
* **soft** — total driving seconds.

The solver is the shared engine in `include/optimaru/`, with two PDP-specific extensions
registered on top of it: a pair-aware move operator (relocates a whole request
and repairs broken pairs) and a pair-aware ruin operator (kicks whole requests,
never half of one). Both are on by default; `pair_moves` / `pair_ruin` turn
them off to measure what they are worth.
"""

from ._core import (
    AcceptorType,
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
    list_constraints,
)
from .models import solve_request

__all__ = [
    "AcceptorType",
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
    "list_constraints",
    "constraint_costs",
    "solve_request",
]
