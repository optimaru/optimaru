"""Assembly-line balancing — Python wrapper of the C++ solver on the optimaru engine.

Simple assembly-line balancing (SALBP-1) with zoning and station domains: one
assignment-shape decision variable (`Task.station`, a station index, -1 = unassigned)
puts every task on one station of the line.

Score, at the default weights:
  hard = -(capacity overflow + reversed precedences + stations beyond max_stations
           + station-domain distance + missed fixed stations + forbidden stations hit
           + extra stations of same-station groups + different-station pairs together)
  soft = -(stations used)                       # workload_imbalance_cost is opt-in (weight 0)
The problem carries no weights: change one through a constraint selection, e.g.
``solve(problem, constraints=[*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)])``
(see `optimaru.assembly_line_balancing.constraints`). `constraint_info()` lists every constraint
with its default level and weight.

Basic usage::

    import optimaru.assembly_line_balancing as alb
    from optimaru.assembly_line_balancing.models import AssemblyLineBalancingProblemModel

    problem = AssemblyLineBalancingProblemModel.model_validate(data)
    solved = alb.solve(problem, time_limit_seconds=5.0, random_seed=42)   # the model: a new, solved model
    core = problem.to_core(); alb.solve(core)                         # or the engine object, in place

`solve`, `calculate_score` and `constraint_costs` take the pydantic model or the engine
object (`AssemblyLineBalancingProblem`, `models.AssemblyLineBalancingProblemModel.to_core()`).
"""

from __future__ import annotations

import copy
from typing import Any, TypeVar

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


def constraint_costs(problem, constraints=None) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight / exponent / offset / level.

    ``{key: {"costs": [...], "weight", "exponent", "offset", "clip", "global",
    "level", "reward", "level_flippable", "name", "label", "default_weight",
    "default_level"}}`` in `list_constraints()` order: one cost per station
    (``0 .. station_count - 1``) for the per-station constraints, one total for the global
    ones; ``max_stations_cost`` and ``forbidden_task_stations_cost`` add ``"units"``, the
    stations they evaluate. None = the default constraints; a pick list (as for `solve`)
    scores exactly those picks, each entry carrying its pick's knobs, the k-th pick of a
    name keyed ``"<name>#<k>"`` from the second on. `constraints.score()` turns it into a
    score.
    """
    return _constraint_costs(_core_of(problem), constraints)


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


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


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
    """Solve `problem` and return the solved problem.

    `problem` is the engine object (solved in place; the server path is ``to_core()`` ->
    ``solve`` -> ``from_core()``) or the pydantic model (converted, solved, and returned as a new
    model with the stations, results and ``warm_start=False`` filled in; the one passed in is left as
    it was). The score is on the result:
    ``.score`` (``.hard``, ``.soft``) of the engine object, ``score_hard`` / ``score_soft`` of
    the model.

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS on).
    Explicit keyword arguments override config. A zero time budget
    (``time_limit_seconds=0``) returns the construction, or a complete warm-start seed,
    unchanged; ``SolverConfig.step_count_limit = N`` bounds the search to N steps (0 = no
    step limit). ``stop_at_first_feasible`` stops as soon as hard 0 is reached.

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly (see `optimaru.assembly_line_balancing.constraints`);
    only the listed constraints are scored, a name picked several times once per pick, and the
    solve is single-start (``parallel_solver_count`` is ignored there). None uses every default
    constraint at its default weight::

        from optimaru.assembly_line_balancing.constraints import get_constraints
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
        cfg.best_score_limit = (0, float("-inf"))
    if full_assert:
        cfg.full_assert = True
    if on_new_best is not None:
        cfg.on_new_best = on_new_best

    core = _core_of(problem)
    if constraints is None:
        Solver(cfg).solve(core)
    else:
        Solver(cfg).solve_with_constraints(core, constraints)
    if isinstance(problem, AssemblyLineBalancingProblemModel):
        # A model is not solved in place: the caller's stays the problem it was.
        solved = problem.model_copy(deep=True)
        solved._update_from_core(core)
        return solved
    return problem
