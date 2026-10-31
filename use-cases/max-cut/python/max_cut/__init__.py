"""Max-Cut — Python wrapper of the C++ solver on the optimaru engine.

Weighted Max-Cut with fixed vertex sides, same-side and different-side pairs,
side-size bounds and an optional balance term: one BASIC planning variable (the
side, 0 or 1) per vertex; a fixed vertex is pinned and never moves.

Score (HardSoftScore), at the default weights:
  hard = -(fixed vertices off their side + split same-side pairs
           + different-side pairs left together + side-size shortfall/excess)
  soft = +(cut weight)                         # cut_weight_cost is a reward
(`partition_imbalance_cost` is an opt-in objective: default weight 0.) The
problem carries no weights: change one through a constraint selection, e.g.
``solve(problem, constraints=[*get_constraints()[:4], PartitionImbalanceCost(weight=2), cut_weight_cost])``
(see `max_cut.constraints`). `constraint_info()` lists every constraint with its
default level and weight.

Basic usage::

    import max_cut
    from max_cut.models import MaxCutProblemModel

    problem = MaxCutProblemModel.model_validate(data)
    max_cut.solve(problem, time_limit_seconds=5.0, random_seed=42)   # the model, in place
    core = problem.to_core(); max_cut.solve(core)                     # or the engine object

`solve`, `calculate_score` and `constraint_costs` take the pydantic model or the engine
object (`MaxCutProblem`, `models.MaxCutProblemModel.to_core()`). `calculate_score` and
`constraint_costs` need a complete partition (every vertex on side 0 or 1) and raise
ValueError on a partial one; `solve` fills in the missing sides.
"""

from __future__ import annotations

import copy
from typing import Any

from ._core import (
    ConstraintInfo,
    Edge,
    MaxCutProblem,
    Score,
    Solver,
    SolverConfig,
    VertexPair,
    constraint_info,
    list_constraints,
)
from ._core import calculate_score as _calculate_score
from ._core import constraint_costs as _constraint_costs
from .constraints import score as _score
from .models import MaxCutProblemModel, with_initial_solution

__all__ = [
    "ConstraintInfo",
    "Edge",
    "MaxCutProblem",
    "MaxCutProblemModel",
    "Score",
    "Solver",
    "SolverConfig",
    "VertexPair",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "with_initial_solution",
]


def _core_of(problem) -> MaxCutProblem:
    if isinstance(problem, MaxCutProblemModel):
        return problem.to_core()
    if isinstance(problem, MaxCutProblem):
        return problem
    raise TypeError(f"problem must be a MaxCutProblemModel or a MaxCutProblem: {type(problem)!r}")


def _complete_core_of(problem) -> MaxCutProblem:
    """The engine object of `problem`, refusing a partial partition.

    0.1.x scored a vertex missing from `partition` as a side of its own (for the cut
    and the pairs) and as side 0 (for the sizes); the engine stores -1 for it, which
    has other rules. Rather than change numbers silently, scoring needs every vertex
    on a side.
    """
    core = _core_of(problem)
    if not core.is_complete():
        sides = core.assignment()
        missing = [vertex for vertex, side in zip(core.vertex_ids, sides, strict=True) if side < 0]
        raise ValueError(
            f"the partition is partial: {len(missing)} vertex(es) without a side, e.g. {missing[:5]}; "
            "scoring needs every vertex on side 0 or 1 (solve fills them in)"
        )
    return core


def constraint_costs(problem) -> dict[str, dict[str, Any]]:
    """Raw cost of every default constraint, before weight / exponent / level.

    ``{name: {"costs": [...], "weight", "exponent", "hard", "global", "level", "reward",
    "level_flippable", "name", "label"}}`` in `list_constraints()` order. The four unit
    constraints (fixed, same-side, different-side, size bounds) report ``[side 0, side 1]``
    (0.1.x listed one 0/1 term per fixed vertex / pair; the sums are equal); the imbalance
    and the cut one total each. `constraints.score()` turns it into a score. Raises
    ValueError on a partial partition.
    """
    return _constraint_costs(_complete_core_of(problem))


def calculate_score(problem, constraints=None):
    """Score the current partition: the default constraints (the C++ full recompute) or a selection.

    With ``constraints`` the score is ``constraints.score(constraint_costs(problem), constraints)``
    (a ``constraints.Score(hard, soft)`` named tuple); without, the engine `Score`. A model gets its
    ``cut_weight``, ``score_hard`` and ``score_soft`` filled in; an engine object its ``score``
    (default constraints only). Raises ValueError on a partial partition.
    """
    core = _complete_core_of(problem)
    result = _calculate_score(core) if constraints is None else _score(_constraint_costs(core), constraints)
    if isinstance(problem, MaxCutProblemModel):
        problem.cut_weight = core.cut_weight()
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
    ``solve`` -> ``from_core()``) or the pydantic model (converted, solved, and the partition,
    cut weight, score and ``warm_start=False`` written back into it).

    The start partition is the 0.1.x one: vertex i takes side ``i % 2``, a fixed vertex its
    fixed side, and with ``warm_start`` the given partition overrides both (see
    `with_initial_solution`). Under exact size bounds (``min_partition_size ==
    max_partition_size``) the search only swaps sides, so the start's side sizes are kept.

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS on).
    Explicit keyword arguments override config. A zero time budget
    (``time_limit_seconds=0``) returns the start partition unchanged, scored;
    ``SolverConfig.step_count_limit = N`` bounds the search to N steps (0 = no step
    limit). ``stop_at_first_feasible`` stops as soon as hard 0 is reached.
    ``on_new_best(step, score)`` is called on every new best solution.

    ``constraints`` picks the cost function directly (see `max_cut.constraints`); only the
    listed constraints are scored and the solve is single-start (``parallel_solver_count``
    is ignored there). None uses every default constraint at its default weight::

        from max_cut.constraints import get_constraints
        max_cut.solve(problem, constraints=get_constraints())
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
    if isinstance(problem, MaxCutProblemModel):
        problem._update_from_core(core)
    return core.score
