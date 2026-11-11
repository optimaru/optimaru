"""Vehicle routing — C++ core with Python helpers.

`Location`, `Visit`, `Vehicle`, `VehicleRoutePlan`, `Score`, `Solver`,
`SolverConfig`, `AcceptorType`, `OperatorSelectionType` and `calculate_score`
come from the compiled C++ core (`_core`). Each vehicle's ordered
`visit_indices` list (its route) is the decision; a visit
missing from every route is unassigned (penalized on the medium score level)
and ends up in `plan.unassigned_visit_indices` after solving. `solve(plan, ...)`
solves a plan in place and returns it; `solve_request` does the same for a
`models` request.

The solver is the shared engine in `optimaru/cpp/optimaru/`: a cheapest-insertion construction
heuristic, then local search over relocate / swap / 2-opt / or-opt / tail-swap
moves, interleaved with adaptive ruin & recreate (ALNS) — random, related,
worst, SISR substring and whole-route ruins under an adaptive operator
selection scheme, repaired by regret-k insertion. Every ALNS knob is on
`SolverConfig`. Destroy-and-repair runs as an acceptor-judged move by default;
the unconditional stagnation kick is off (`ruin_recreate = True` turns it on —
on a tightly loaded fleet it stalls the search, see `Solver::Config`). Two more
strategies run by default on top of the move mix: an ejection chain
(`ejection_chain_weight`) for the slack shifts a tightly loaded fleet needs,
and an iterated local search (`iterated_local_search_weight`: ruin a few
visits, repair, descend to a local optimum) that does most of the work on
large instances; with it the nearby neighbourhood is 25 visits
(`driving_time_nearby_size`). See `Solver::Config` for the measurements behind
each default.

Score convention (hard / medium / soft), at the default weights:

* **hard** — capacity excess (`vehicle_capacity_cost`) + time-window lateness
  (`max_end_time_cost`), plus the optional route policies
  (`max_travel_time_cost`, `max_route_duration_cost`,
  `vehicle_max_end_time_cost`, `max_stops_cost`, `max_route_radius_cost`).
  0 = feasible.
* **medium** — -(penalty minutes of unassigned visits)
  (`unassigned_visits_cost`); the engine folds it into soft as
  ``medium * 10^9``.
* **soft** — -(total driving seconds) (`travel_time_cost`), plus
  `min_capacity_utilization_cost`, `vehicle_fixed_cost` and the opt-in
  weight-0 objectives.

Visits that share a `group` (`Visit.group_index`, `VisitModel.group`) are one job:
they ride one vehicle together or all stay unassigned (`visit_group_cost`, hard),
and `after` (`Visit.after_visit_indices`) puts a partial order on them
(`visit_order_cost`, hard). A group operator moves a group between vehicles as one
unit (`group_ruin_recreate`).

Compared lexicographically: hard dominates medium dominates soft.
`constraint_info()` lists every constraint with its default level and weight;
`optimaru.vehicle_routing.constraints` picks and re-weights them.
"""

from .._solve import solve_in_place, solver_config
from ._core import (
    AcceptorType,
    ConstraintInfo,
    Location,
    OperatorSelectionType,
    Score,
    Solver,
    SolverConfig,
    Vehicle,
    VehicleRoutePlan,
    Visit,
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
    "Location",
    "OperatorSelectionType",
    "Score",
    "Solver",
    "SolverConfig",
    "Vehicle",
    "VehicleRoutePlan",
    "Visit",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "solve_request",
    "with_initial_solution",
]


def solve(
    plan: VehicleRoutePlan,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> VehicleRoutePlan:
    """High-level API that solves `plan` in place and returns it.

    ``config`` becomes the base settings (`SolverConfig`; None = engine defaults,
    a 30 s budget). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached, ``full_assert``
    re-verifies every incremental score against a full recompute (a debugging aid,
    much slower), and ``on_new_best(step, score)`` is called on every new best.

    ``constraints`` picks the cost function directly (see
    `optimaru.vehicle_routing.constraints`). Only the listed constraints are scored;
    None uses the default constraints at their default weights::

        from optimaru.vehicle_routing.constraints import get_constraints
        vrp.solve(plan, constraints=get_constraints())

    A plan that puts visits in groups (`Visit.group_index`) is scored with the default
    constraints even when ``constraints`` is None: `visit_group_cost` and
    `visit_order_cost` are among them, and they are not among the four core rules a
    plan is otherwise scored on.

    The score is on the result: ``plan.score``. `solve_request` is the same solve for
    a `models` request (JSON in, JSON out).
    """
    if not isinstance(plan, VehicleRoutePlan):
        raise TypeError(
            f"plan must be an instance of VehicleRoutePlan: {type(plan)!r} (models.solve_request solves a models.RoutePlanRequest)"
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
    if constraints is None and any(visit.group_index >= 0 for visit in plan.visits):
        constraints = get_constraints()
    solve_in_place(Solver(cfg), plan, constraints)
    return plan
