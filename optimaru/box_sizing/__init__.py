"""Box sizing — Python wrapper of the C++ solver on the optimaru engine.

Which box sizes to stock: every parcel (the packed content of one shipment, by its bounding
size) ships in one of the candidate boxes, only a limited number of distinct boxes may be
in use, and the least air is shipped. One assignment-shape decision variable (`Parcel.box`,
a box index, -1 = unassigned) picks the box of each parcel; a parcel fits a box when its
three sides, sorted, are each at most the box's sorted sides (any rotation).

Score, at the default weights (1 each):
  hard = -(length by which parcels exceed their box, over the sorted sides   # parcel_fit_cost
           + parcel weight above the box's max_weight                        # box_weight_cost
           + distinct boxes in use above max_box_types)                      # box_type_limit_cost
  soft = -(quantity x (box volume - parcel volume), over the parcels         # void_volume_cost
           + quantity x the box's cost, over the parcels)                    # box_cost
`box_type_cost` (the number of boxes in use, soft) is opt-in at weight 0: a weight on it
is the fixed cost of stocking one more size. The problem carries no weights: change one
through a constraint selection (see `optimaru.box_sizing.constraints`).
`constraint_info()` lists every constraint with its default level and weight.

Basic usage::

    import optimaru.box_sizing as bs

    problem = bs.generate_demo_data()               # models.BoxSizingProblemModel
    solved = bs.solve(problem, time_limit_seconds=2.0, random_seed=42)
    print(solved.box_types_used, solved.void_volume, solved.fill_ratio)
    print([(box.id, box.parcels) for box in solved.boxes if box.parcels])

`solve` also takes the compiled `BoxSizingProblem` and solves it in place. The JSON
contract is `optimaru.box_sizing.models` (pydantic).
"""

from typing import TypeVar

from .. import _api
from .._config import grouped as _grouped
from .._solve import check_parameters, solve_in_place, solver_config
from ._core import (
    Box,
    BoxSizingProblem,
    ConstraintInfo,
    Parcel,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
)
from .constraints import get_constraints as _get_constraints
from .demo import generate_demo_data
from .models import BoxSizingProblemModel, solve_request, with_initial_solution
from .models import parameter_uses as _parameter_uses

# The settings by stage of the solve (optimaru._config): config.stop, config.custom_moves, ...
SolverConfig = _grouped(SolverConfig, module=__name__)

__all__ = [
    "Box",
    "BoxSizingProblem",
    "ConstraintInfo",
    "Parcel",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "generate_demo_data",
    "list_constraints",
    "solve",
    "solve_request",
    "with_initial_solution",
]


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


@_api.model_solve("box-sizing", BoxSizingProblemModel)
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
    """High-level API that solves `problem` and returns it.

    `problem` is a `BoxSizingProblem` (solved in place and returned) or a
    `models.BoxSizingProblemModel` (converted, solved, and returned as a new model with
    the outputs filled in; the one passed in is left as it was).

    The score is on the result: ``solve(problem).score`` (``.hard``, ``.soft``), or
    ``score_hard`` / ``score_soft`` on a model.

    ``config`` becomes the base settings (None = the defaults: a 30 s budget). Explicit
    keyword arguments override config. ``time_limit_seconds=0`` returns the greedy
    construction as it is. ``stop_at_first_feasible`` stops as soon as hard 0 is reached.

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly
    (see `optimaru.box_sizing.constraints`). Only the listed constraints are scored;
    None uses the default constraints at their default weights::

        from optimaru.box_sizing.constraints import BoxTypeCost, get_constraints
        bs.solve(problem, constraints=[
            BoxTypeCost(weight=2_000_000) if spec.name == "box_type_cost" else spec
            for spec in get_constraints()
        ])

    A model that sets a parameter whose costs the selection leaves out is refused with
    ValueError (`models.parameter_uses`: `max_weight`, `cost`, `required`,
    `max_box_types`); an engine object is solved as it is.

    Raises ValueError for a parcel that fits none of the candidate boxes (by size and
    weight), for more required boxes than `max_box_types`, and for facts out of range.
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
    if isinstance(problem, BoxSizingProblem):
        solve_in_place(Solver(cfg), problem, constraints)
        return problem
    if isinstance(problem, BoxSizingProblemModel):
        core = problem.to_core()
        constraints = check_parameters(
            _parameter_uses(problem), constraints, _get_constraints, lambda picks: constraint_costs(core, picks)
        )
        solve_in_place(Solver(cfg), core, constraints)
        return BoxSizingProblemModel.from_core(core)
    raise TypeError(f"problem must be a BoxSizingProblem or a models.BoxSizingProblemModel: {type(problem)!r}")
