"""Container loading — Python wrapper of the C++ solver on the optimaru engine.

Three-dimensional loading of boxes into containers: pallets, rack bays, a vehicle's cargo
space, shipping cartons. The decision is an order: every container has a list of items,
its loading order (`Container.items`, item indices). Where each item then stands is
derived from that order by a placement decoder, so positions are outputs:
`Item.container`, `x`, `y`, `z` and the placed size.

Score, at the default weights (1 each; the two opt-in costs at 0):
  hard = -(items that found no place                    # unplaced_item_cost
           + weight above a container's max_weight      # container_weight_cost
           + weight on an item above max_load_on_top    # stacking_load_cost
           + stacking rules and layer limits broken     # stacking_rule_cost
           + items blocking one that leaves earlier     # unloading_order_cost
           + centre of gravity beyond its limits        # balance_limit_cost
           + pairs a segregation rule keeps apart       # segregation_cost
           + all-or-none groups loaded in part          # incomplete_group_cost
           + one-container groups in several)           # single_container_cost
  (an unplaced item counts its priority, an optional one in optional_item_cost)
  soft = -(cost of the containers in use                # used_container_cost
           + optional items left out                    # optional_item_cost
           + containers a group is in beyond its first) # group_split_cost
           (+ used_length_cost, load_balance_cost when given a weight)
hard 0 == feasible. The problem carries no weights: change one through a constraint
selection (see `optimaru.container_loading.constraints`). `constraint_info()` lists every
constraint with its default level and weight.

Basic usage::

    import optimaru.container_loading as cl

    problem = cl.ContainerLoadingProblem(
        containers=[cl.Container("pallet-1", 1200, 800, 1500), cl.Container("pallet-2", 1200, 800, 1500)],
        items=[cl.Item(f"box-{i}", 400, 300, 300, weight=6000) for i in range(30)],
    )
    cl.solve(problem, time_limit_seconds=2.0, random_seed=0)
    print(problem.score, problem.containers_used())
    for item in problem.items:
        print(item.id, item.container, (item.x, item.y, item.z))

`problem.items` and `problem.containers` return copies: write loading orders with
``problem.set_loading([...])``. The JSON contract is `optimaru.container_loading.models`
(pydantic).
"""

from typing import TypeVar

from .. import _api
from .._config import grouped as _grouped
from .._solve import check_parameters, solve_in_place, solver_config
from ._core import (
    ConstraintInfo,
    Container,
    ContainerLoadingProblem,
    Item,
    Rotation,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    constraint_info,
    generate_demo_data,
    list_constraints,
    update_placements,
)
from .constraints import get_constraints as _get_constraints
from .models import ContainerLoadingProblemModel, solve_request, with_initial_solution
from .models import parameter_uses as _parameter_uses

# The settings by stage of the solve (optimaru._config): config.stop, config.alns, ...
SolverConfig = _grouped(SolverConfig, module=__name__)

__all__ = [
    "ConstraintInfo",
    "Container",
    "ContainerLoadingProblem",
    "Item",
    "Rotation",
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
    "update_placements",
    "with_initial_solution",
]


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


@_api.model_solve("container-loading", ContainerLoadingProblemModel)
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

    `problem` is a `ContainerLoadingProblem` (solved in place and returned) or a
    `models.ContainerLoadingProblemModel` (converted, solved, and returned as a new model;
    the one passed in is left as it was).

    The score is on the result: ``solve(problem).score`` (``.hard``, ``.soft``), or
    ``score_hard`` and ``score_soft`` on a model.

    The loading orders the problem comes with are the starting point: the solve keeps them
    and constructs only the items no container lists (a model's `initial_items`, or the
    `items` of a solved model).

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS
    on). Explicit keyword arguments override config. ``stop.time_limit_seconds = 0`` on
    the config skips the search: the construction comes back as it is.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached;
    ``on_new_best(step, score)`` is called on every new best solution.

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly (see
    `optimaru.container_loading.constraints`). Only the listed constraints are scored, a
    name picked several times once per pick; None uses all fourteen default constraints at
    their default weights. With constraints the solve is single-start
    (``parallel_solver_count`` is ignored)::

        from optimaru.container_loading.constraints import get_constraints
        cl.solve(problem, constraints=get_constraints())

    A model that sets a parameter whose costs the selection leaves out is refused with
    ValueError (`models.parameter_uses`); an engine object is solved as it is.
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
    if isinstance(problem, ContainerLoadingProblem):
        solve_in_place(Solver(cfg), problem, constraints)
        return problem
    if isinstance(problem, ContainerLoadingProblemModel):
        core = problem.to_core()
        constraints = check_parameters(
            _parameter_uses(problem), constraints, _get_constraints, lambda picks: constraint_costs(core, picks)
        )
        solve_in_place(Solver(cfg), core, constraints)
        solved = ContainerLoadingProblemModel.from_core(core)
        for container, given in zip(solved.containers, problem.containers):
            container.initial_items = list(given.initial_items)  # an input: it stays on the result
        return solved
    raise TypeError(
        f"problem must be a ContainerLoadingProblem or a models.ContainerLoadingProblemModel: {type(problem)!r}"
    )
