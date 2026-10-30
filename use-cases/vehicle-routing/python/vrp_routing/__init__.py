"""Vehicle routing — C++ core with Python helpers.

`Location`, `Visit`, `Vehicle`, `VehicleRoutePlan`, `Score`, `Solver`,
`SolverConfig`, `AcceptorType`, `OperatorSelectionType` and `calculate_score`
come from the compiled C++ core (`_core`). The plan is the planning solution:
each vehicle's ordered `visit_indices` list is the planning variable; a visit
missing from every route is unassigned (penalized on the medium score level)
and ends up in `plan.unassigned_visit_indices` after solving.

The solver is the shared engine in `include/optimaru/`: a cheapest-insertion construction
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
(`driving_time_nearby_size`, was 50). Both change the search trajectory for a
given seed compared with earlier releases; see `Solver::Config` for the
measurements behind each default.

Score convention (HardMediumSoft, as in the reference VRP example model):
hard = capacity excess + time-window lateness (0 = feasible), medium =
-(service minutes of unassigned visits), soft = -(total driving seconds).
Compared lexicographically: hard dominates medium dominates soft.
"""

from ._core import (
    AcceptorType,
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
    list_constraints,
)

__all__ = [
    "AcceptorType",
    "Location",
    "OperatorSelectionType",
    "Visit",
    "Vehicle",
    "VehicleRoutePlan",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "list_constraints",
    "constraint_costs",
    "solve_request",
    "with_initial_solution",
]


def solve_request(request, *, constraints=None):
    """Solve a `models.RoutePlanRequest` end-to-end; see `models.solve_request`."""
    # Lazy import: pydantic is an optional dependency.
    from . import models

    return models.solve_request(request, constraints=constraints)


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    See `models.with_initial_solution`.
    """
    from . import models

    return models.with_initial_solution(problem, solution)
