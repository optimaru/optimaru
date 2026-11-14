"""Quadratic assignment — Python wrapper of the C++ solver on the optimaru engine.

Place n facilities on n locations, one each, minimizing the flow between every
pair of facilities times the distance between their locations (plus an optional
direct facility-location cost), with pinned and forbidden placements and
location capacities as hard constraints. One assignment-shape decision variable
(`Facility.location`) per facility; the search only swaps two facilities'
locations, so every state is a permutation.

Score, at the default weights (all 1):
  hard = -(pinned locations not holding their facility
           + facilities at a forbidden location
           + demand above location capacities)
  soft = -(direct assignment cost + flow-distance cost)
The problem carries no weights: change one through a constraint selection, e.g.::

    picks = [FlowDistanceCost(weight=2) if spec.name == "flow_distance_cost" else spec for spec in get_constraints()]
    solve(problem, constraints=picks)

See `optimaru.quadratic_assignment.constraints`; `constraint_info()` lists every constraint with its
default level and weight.

Basic usage::

    import optimaru.quadratic_assignment as qap
    from optimaru.quadratic_assignment.models import QuadraticAssignmentProblemModel

    problem = QuadraticAssignmentProblemModel.model_validate(data)
    problem = qap.solve(problem, time_limit_seconds=5.0, random_seed=42)   # a new, solved model
    core = problem.to_core(); qap.solve(core)   # or the engine object, solved in place

`solve`, `calculate_score` and `constraint_costs` take the pydantic model or the
engine object (`QuadraticAssignmentProblem`, `models.QuadraticAssignmentProblemModel.to_core()`).
On a model, `calculate_score` and `constraint_costs` need an assignment that covers
every location (ValueError naming the missing ones); the engine object scores a
partial assignment too (an unassigned facility contributes nothing).
"""

from __future__ import annotations

from typing import Any, TypeVar

from .. import _api
from .._solve import solve_in_place, solver_config
from ._core import (
    AcceptorType,
    ConstraintInfo,
    Facility,
    FixedAssignment,
    ForbiddenAssignment,
    Location,
    QuadraticAssignmentProblem,
    Score,
    Solver,
    SolverConfig,
    constraint_info,
    list_constraints,
)
from ._core import calculate_score as _calculate_score
from ._core import constraint_costs as _constraint_costs
from .constraints import score as _score
from .models import QuadraticAssignmentProblemModel, solve_request, with_initial_solution

__all__ = [
    "AcceptorType",
    "ConstraintInfo",
    "Facility",
    "FixedAssignment",
    "ForbiddenAssignment",
    "Location",
    "QuadraticAssignmentProblem",
    "QuadraticAssignmentProblemModel",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "solve_request",
    "with_initial_solution",
]


def _core_of(problem, *, complete: bool = False) -> QuadraticAssignmentProblem:
    if isinstance(problem, QuadraticAssignmentProblemModel):
        if complete and (missing := problem.missing_locations()):
            raise ValueError(f"assignment must cover every location; missing: {missing}")
        return problem.to_core()
    if isinstance(problem, QuadraticAssignmentProblem):
        return problem
    raise TypeError(
        f"problem must be a QuadraticAssignmentProblemModel or a QuadraticAssignmentProblem: {type(problem)!r}"
    )


def constraint_costs(problem, constraints=None) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight / exponent / offset / level.

    ``{key: {"costs": [...], "weight", "exponent", "offset", "clip", "global",
    "level", "reward", "level_flippable", "name", "label", ...}}``: None = the default
    constraints in `list_constraints()` order; a pick list (as for `solve`) scores exactly
    those picks, each entry carrying its pick's knobs, the k-th pick of a name keyed
    ``"<name>#<k>"``. The three side constraints list one raw cost per location (location
    order) and their
    evaluated locations as ``"units"`` (the fixed, the forbidden-row and the capacitated
    locations: the only ones a literal offset charges); `assignment_cost` and
    `flow_distance_cost` are one total each. `constraints.score()` turns it into a score.
    A model needs an assignment covering every location.
    """
    return _constraint_costs(_core_of(problem, complete=True), constraints)


def calculate_score(problem, constraints=None):
    """Score the current assignment: the default constraints (the C++ full recompute) or a selection.

    With ``constraints`` the score is ``constraints.score(constraint_costs(problem), constraints)``
    (a ``constraints.Score(hard, soft)`` named tuple); without, the engine `Score`. A model needs
    an assignment covering every location (ValueError naming the missing ones) and gets its
    ``total_cost``, ``score_hard`` and ``score_soft`` filled in; an engine object its ``score``
    (default constraints only).
    """
    core = _core_of(problem, complete=True)
    result = _calculate_score(core) if constraints is None else _score(_constraint_costs(core), constraints)
    if isinstance(problem, QuadraticAssignmentProblemModel):
        problem.total_cost = core.total_cost()
        problem.score_hard, problem.score_soft = result.hard, result.soft
    return result


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


@_api.model_solve("quadratic-assignment", QuadraticAssignmentProblemModel)
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
    """Solve `problem` and return it, solved.

    `problem` is the engine object (solved in place; the server path is ``to_core()`` ->
    ``solve`` -> ``from_core()``) or the pydantic model (converted, solved, and returned as a
    new model with the assignment, ``total_cost``, score and ``warm_start=False`` filled in; the
    one passed in is left as it was).

    The score is on the result: ``problem.score`` (`Score`) on the engine object,
    ``score_hard`` / ``score_soft`` on the model.

    The search starts from the identity (facility i at location i) or, with
    ``warm_start`` (see `with_initial_solution`), from the problem's assignment, which must
    then be a full bijection; a seed is used once.

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, late
    acceptance over swap moves). Explicit keyword arguments override config. A zero time
    budget (``time_limit_seconds=0``) returns the start unchanged; ``SolverConfig.step_count_limit
    = N`` bounds the search to N steps (0 = no step limit). ``stop_at_first_feasible`` stops as
    soon as hard 0 is reached. ``on_new_best(step, score)`` is called on every new best solution.

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly (see `optimaru.quadratic_assignment.constraints`);
    only the listed constraints are scored, a name picked several times once per pick, and
    the solve is single-start (``parallel_solver_count`` is ignored there). None uses every
    default constraint at its default weight::

        from optimaru.quadratic_assignment.constraints import get_constraints
        problem = quadratic_assignment.solve(problem, constraints=get_constraints())
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
    core = _core_of(problem)
    solve_in_place(Solver(cfg), core, constraints)
    if isinstance(problem, QuadraticAssignmentProblemModel):
        # A model is not solved in place: the caller's stays the problem it was. A copy
        # updated from the core, not from_core(): the copy keeps the model's own fields
        # and their order, which the core does not carry.
        solved = problem.model_copy(deep=True)
        solved._update_from_core(core)
        return solved
    return problem
