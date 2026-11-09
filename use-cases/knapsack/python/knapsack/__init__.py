"""Knapsack — Python wrapper of the C++ solver on the optimaru engine.

The 0/1 knapsack with multidimensional resource capacities, mandatory and
forbidden items, selection-group bounds and item dependencies: one assignment-shape
planning variable (`Item.selected`, 1 = selected, 0 = not) per item.

Score, at the default weights:
  hard = -(weight over capacity + resource overflow + unselected mandatory items
           + selected forbidden items + selection-group shortfall/excess
           + unmet item dependencies)
  soft = +(selected value)                       # selected_value_cost is a reward
The problem carries no weights: change one through a constraint selection, e.g.
``solve(problem, constraints=[*get_constraints()[:-1], SelectedValueCost(weight=2)])``
(see `knapsack.constraints`). `constraint_info()` lists every constraint with its
default level and weight.

Basic usage::

    import knapsack
    from knapsack.models import KnapsackProblemModel

    problem = KnapsackProblemModel.model_validate(data)
    knapsack.solve(problem, time_limit_seconds=5.0, random_seed=42)   # the model, in place
    core = problem.to_core(); knapsack.solve(core)                     # or the engine object

`solve`, `calculate_score` and `constraint_costs` take the pydantic model or the engine
object (`KnapsackProblem`, `models.KnapsackProblemModel.to_core()`).
"""

from __future__ import annotations

import copy
from typing import Any

from ._core import (
    ConstraintInfo,
    Item,
    KnapsackProblem,
    Resource,
    Score,
    SelectionGroup,
    Solver,
    SolverConfig,
    constraint_info,
    list_constraints,
)
from ._core import calculate_score as _calculate_score
from ._core import constraint_costs as _constraint_costs
from .constraints import score as _score
from .models import KnapsackProblemModel, with_initial_solution

__all__ = [
    "ConstraintInfo",
    "Item",
    "KnapsackProblem",
    "KnapsackProblemModel",
    "Resource",
    "Score",
    "SelectionGroup",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "with_initial_solution",
]


def _core_of(problem) -> KnapsackProblem:
    if isinstance(problem, KnapsackProblemModel):
        return problem.to_core()
    if isinstance(problem, KnapsackProblem):
        return problem
    raise TypeError(f"problem must be a KnapsackProblemModel or a KnapsackProblem: {type(problem)!r}")


def constraint_costs(problem, constraints=None) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight / exponent / offset / level.

    ``{key: {"costs": [total], "weight", "exponent", "offset", "clip", "hard", "global",
    "level", "reward", "level_flippable", "name", "label", ...}}``: None = the default
    constraints in `list_constraints()` order; a pick list (as for `solve`) scores exactly
    those picks, each entry carrying its pick's knobs, the k-th pick of a name keyed
    ``"<name>#<k>"``. Every knapsack constraint is global, so ``costs`` holds one raw total
    (0.1.x listed one term per resource / entry / group / pair). `constraints.score()`
    turns it into a score.
    """
    return _constraint_costs(_core_of(problem), constraints)


def calculate_score(problem, constraints=None):
    """Score the current selection: the default constraints (the C++ full recompute) or a selection.

    With ``constraints`` the score is ``constraints.score(constraint_costs(problem), constraints)``
    (a ``constraints.Score(hard, soft)`` named tuple); without, the engine `Score`. A model gets its
    ``total_weight``, ``total_value``, ``score_hard`` and ``score_soft`` filled in; an engine object
    its ``score`` (default constraints only).
    """
    core = _core_of(problem)
    result = _calculate_score(core) if constraints is None else _score(_constraint_costs(core), constraints)
    if isinstance(problem, KnapsackProblemModel):
        problem.total_weight = core.total_weight()
        problem.total_value = core.total_value()
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
    ``solve`` -> ``from_core()``) or the pydantic model (converted, solved, and the selection,
    totals, score and ``warm_start=False`` written back into it).

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS on).
    Explicit keyword arguments override config. A zero time budget
    (``time_limit_seconds=0``) returns the greedy construction, or a warm-start seed,
    unchanged; ``SolverConfig.step_count_limit = N`` bounds the search to N steps (0 = no
    step limit). ``stop_at_first_feasible`` stops as soon as hard 0 is reached.
    ``on_new_best(step, score)`` is called on every new best solution.

    ``constraints`` picks the cost function directly (see `knapsack.constraints`); only the
    listed constraints are scored, a name picked several times once per pick, and the solve
    is single-start (``parallel_solver_count`` is ignored there). None uses every default
    constraint at its default weight::

        from knapsack.constraints import get_constraints
        knapsack.solve(problem, constraints=get_constraints())
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
    if on_new_best is not None:
        cfg.on_new_best = on_new_best

    core = _core_of(problem)
    if constraints is None:
        Solver(cfg).solve(core)
    else:
        Solver(cfg).solve_with_constraints(core, constraints)
    if isinstance(problem, KnapsackProblemModel):
        problem._update_from_core(core)
    return core.score
