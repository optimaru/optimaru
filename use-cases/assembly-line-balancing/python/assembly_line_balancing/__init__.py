"""Assembly-line balancing — Python wrapper of the C++ solver on the optimaru engine.

Simple assembly-line balancing (SALBP-1) with zoning and station domains: one
BASIC planning variable (`Task.station`, a station index, -1 = unassigned)
puts every task on one station of the line.

Score (HardSoftScore), at the default weights:
  hard = -(capacity overflow + reversed precedences + stations beyond max_stations
           + station-domain distance + missed fixed stations + forbidden stations hit
           + extra stations of same-station groups + different-station pairs together)
  soft = -(stations used)                       # workload_imbalance_cost is opt-in (weight 0)
The problem carries no weights: change one through a constraint selection, e.g.
``solve(problem, constraints=[*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)])``
(see `assembly_line_balancing.constraints`). `constraint_info()` lists every constraint
with its default level and weight.

Basic usage::

    import assembly_line_balancing as alb
    from assembly_line_balancing.models import AssemblyLineBalancingProblemModel

    problem = AssemblyLineBalancingProblemModel.model_validate(data)
    alb.solve(problem, time_limit_seconds=5.0, random_seed=42)       # the model, in place
    core = problem.to_core(); alb.solve(core)                         # or the engine object

`solve`, `calculate_score` and `constraint_costs` take the pydantic model or the engine
object (`AssemblyLineBalancingProblem`, `models.AssemblyLineBalancingProblemModel.to_core()`).
"""

from __future__ import annotations

import copy
from typing import Any

from ._core import (
    AssemblyLineBalancingProblem,
    ConstraintInfo,
    Precedence,
    Score,
    Solver,
    SolverConfig,
    Task,
    TaskGroup,
    TaskPair,
    constraint_info,
    list_constraints,
)
from ._core import calculate_score as _calculate_score
from ._core import constraint_costs as _constraint_costs
from .constraints import score as _score
from .models import AssemblyLineBalancingProblemModel, with_initial_solution

__all__ = [
    "AssemblyLineBalancingProblem",
    "AssemblyLineBalancingProblemModel",
    "ConstraintInfo",
    "Precedence",
    "Score",
    "Solver",
    "SolverConfig",
    "Task",
    "TaskGroup",
    "TaskPair",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "with_initial_solution",
]


def _core_of(problem) -> AssemblyLineBalancingProblem:
    if isinstance(problem, AssemblyLineBalancingProblemModel):
        return problem.to_core()
    if isinstance(problem, AssemblyLineBalancingProblem):
        return problem
    raise TypeError(
        f"problem must be an AssemblyLineBalancingProblemModel or an AssemblyLineBalancingProblem: {type(problem)!r}"
    )


def constraint_costs(problem) -> dict[str, dict[str, Any]]:
    """Raw cost of every default constraint, before weight / exponent / level.

    ``{name: {"costs": [...], "weight", "exponent", "hard", "global", "level", "reward",
    "level_flippable", "name", "label"}}`` in `list_constraints()` order: one cost per
    station (``0 .. station_count - 1``) for the per-station constraints, one total for the
    global ones. `constraints.score()` turns it into a score.
    """
    return _constraint_costs(_core_of(problem))


def calculate_score(problem, constraints=None):
    """Score the current stations: the default constraints (the C++ full recompute) or a selection.

    With ``constraints`` the score is ``constraints.score(constraint_costs(problem), constraints)``
    (a ``constraints.Score(hard, soft)`` named tuple); without, the engine `Score`. A model gets its
    ``station_loads``, ``stations_used``, ``score_hard`` and ``score_soft`` filled in; an engine
    object its ``score`` (default constraints only).
    """
    core = _core_of(problem)
    result = _calculate_score(core) if constraints is None else _score(_constraint_costs(core), constraints)
    if isinstance(problem, AssemblyLineBalancingProblemModel):
        problem.station_loads = list(core.station_loads())
        problem.stations_used = core.stations_used()
        problem.score_hard, problem.score_soft = result.hard, result.soft
    return result


def solve(
    problem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> Score:
    """Solve `problem` in place and return the final `Score`.

    `problem` is the engine object (solved in place; the server path is ``to_core()`` ->
    ``solve`` -> ``from_core()``) or the pydantic model (converted, solved, and the stations,
    results and ``warm_start=False`` written back into it).

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS on).
    Explicit keyword arguments override config. A zero time budget
    (``time_limit_seconds=0``) returns the construction, or a complete warm-start seed,
    unchanged; ``SolverConfig.step_count_limit = N`` bounds the search to N steps (0 = no
    step limit). ``stop_at_first_feasible`` stops as soon as hard 0 is reached.

    ``constraints`` picks the cost function directly (see `assembly_line_balancing.constraints`);
    only the listed constraints are scored and the solve is single-start (``parallel_solver_count``
    is ignored there). None uses every default constraint at its default weight::

        from assembly_line_balancing.constraints import get_constraints
        alb.solve(problem, constraints=get_constraints())
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

    core = _core_of(problem)
    if constraints is None:
        Solver(cfg).solve(core)
    else:
        Solver(cfg).solve_with_constraints(core, constraints)
    if isinstance(problem, AssemblyLineBalancingProblemModel):
        problem._update_from_core(core)
    return core.score
