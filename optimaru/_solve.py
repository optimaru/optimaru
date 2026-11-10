"""The steps the use cases' ``solve()`` functions share.

``optimaru.<use case>.solve(problem, *, time_limit_seconds, random_seed,
parallel_solver_count, stop_at_first_feasible, full_assert, on_new_best, config,
constraints)`` is one contract (docs/use-cases/README.md, "One way to solve"). The two
steps that are the same from one use case to the next are here, and a package's
``__init__.py`` calls them::

    from .._solve import solve_in_place, solver_config

    def solve(problem, *, time_limit_seconds=None, ..., config=None, constraints=None):
        cfg = solver_config(SolverConfig, config, time_limit_seconds=time_limit_seconds, ...)
        solve_in_place(Solver(cfg), problem, constraints)
        return problem

Which problem types a use case takes, and what it returns for each of them, stays in
its own ``solve()``. So does the solver call of employee scheduling and school
timetabling, whose constraints path hands the solver a calculator.
"""

import copy
from collections.abc import Callable
from typing import Any, TypeVar

__all__ = ["solve_in_place", "solver_config"]

_Config = TypeVar("_Config")


def solver_config(
    config_type: Callable[[], _Config],
    config: _Config | None = None,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best: Callable[..., Any] | None = None,
    first_feasible_limit: tuple = (0, float("-inf")),
) -> _Config:
    """The ``SolverConfig`` of one ``solve()`` call: ``config`` with the keyword arguments applied.

    ``config`` is the caller's base settings. It is copied, so the overrides never leak
    into it; ``None`` starts from ``config_type()``, the engine defaults. A keyword left
    at its default keeps the base setting:

    - ``time_limit_seconds`` sets ``spent_limit_seconds``;
    - ``random_seed`` and ``parallel_solver_count`` set the fields of the same name;
    - ``stop_at_first_feasible`` sets ``best_score_limit`` to ``first_feasible_limit``.
      The default is hard 0 with no floor on soft, for a (hard, soft) score; a use case
      with more score levels passes its own;
    - ``full_assert`` turns ``full_assert`` on (it never turns it off);
    - ``on_new_best`` sets the callback.
    """
    cfg = config_type() if config is None else copy.copy(config)
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    if stop_at_first_feasible:
        cfg.best_score_limit = first_feasible_limit
    if full_assert:
        cfg.full_assert = True
    if on_new_best is not None:
        cfg.on_new_best = on_new_best
    return cfg


def solve_in_place(solver: Any, problem: Any, constraints: Any = None) -> None:
    """Solve the compiled ``problem`` in place: the default constraints, or the picked ones.

    ``solver`` is the use case's ``Solver(config)``. ``constraints`` is a selection (see
    the use case's ``constraints`` module); ``None`` keeps the default constraints on the
    solver's default path.
    """
    if constraints is None:
        solver.solve(problem)
    else:
        solver.solve_with_constraints(problem, constraints)
