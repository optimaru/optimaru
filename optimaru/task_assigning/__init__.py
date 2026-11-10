"""Task assigning — Python wrapper of the C++ solver on the optimaru engine.

`Customer`/`TaskType`/`Task`/`Employee`/`TaskAssigningSolution`/`Score`/
`Solver`/`SolverConfig` and `calculate_score` come from the compiled C++ core (`_core`).
The decision variable is each `Employee.tasks` — the ordered list of task indices the
employee performs back to back. A task in no list is unassigned.

Derived values (recomputed by `calculate_score`/`Solver.solve`):

    task.employee    == index of the employee owning the list (-1 == unassigned)
    task.start_time  == the first task gets its own min_start_time,
                        later tasks max(end of the previous task, min_start_time)
    duration         == base_duration x affinity multiplier (HIGH 1 / MEDIUM 2 /
                        LOW 3 / NONE 4)

Score convention (four score levels, [1]hard/[3]soft; all penalties),
at the default weights:
  hard[0]  required_skill_cost          required skills the employee lacks
  soft[0]  unassigned_tasks_cost        1 per unassigned task (the medium level)
  soft[1]  squared_makespan_cost        endTime^2 per employee
  soft[2]  {critical|major|minor}_priority_task_end_time_cost
                                        endTime x 4 / x 2 / x 1
Internally it maps onto Score(int64, double) as
`engine.hard = hard * 10**9 + soft0` (the medium level folds below hard) and
`engine.soft = soft1 * 10**7 + soft2` (squared_makespan_cost's default weight is 10**7);
level order preserved, see cpp/include/ta/domain.hpp for the exact rules.
`constraint_info()` lists every constraint with its default level and weight; change one
through a constraint selection (see `optimaru.task_assigning.constraints`).

The high-level entry point is `solve`::

    import optimaru.task_assigning as ta
    from optimaru.task_assigning.demo import generate_demo_data

    solution = generate_demo_data()      # models.TaskAssigningSolutionModel
    result = ta.solve(solution, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_soft0, result.score_soft1, result.score_soft2)

The pydantic mirror (for serialization) is in `optimaru.task_assigning.models` (optional dependency).
"""

from typing import TypeVar

from .._solve import solve_in_place, solver_config
from ._core import (
    Affinity,
    ConstraintInfo,
    Customer,
    Employee,
    Priority,
    Score,
    Solver,
    SolverConfig,
    Task,
    TaskAssigningSolution,
    TaskType,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
)

__all__ = [
    "Affinity",
    "ConstraintInfo",
    "Customer",
    "Employee",
    "Priority",
    "Score",
    "Solver",
    "SolverConfig",
    "Task",
    "TaskAssigningSolution",
    "TaskType",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "with_initial_solution",
]


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    See `models.with_initial_solution`.
    """
    # Lazy import: pydantic is an optional dependency.
    from . import models

    return models.with_initial_solution(problem, solution)


def _build_config(**settings) -> SolverConfig:
    """`solve()`'s `config` and keyword overrides as the `SolverConfig` of the solve."""
    # Stop as soon as hard 0 + every task assigned is reached: -(2**63) on soft1/soft2
    # is "no floor", so no soft total, however low, misses the stop.
    return solver_config(SolverConfig, first_feasible_limit=(0, 0, -(2**63), -(2**63)), **settings)


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


def solve(
    solution: _Problem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config=None,
    constraints=None,
) -> _Problem:
    """High-level API that solves a task assigning problem and returns it in the same shape.

    ``solution`` is a `_core.TaskAssigningSolution` (solved in place and
    returned) or a `models.TaskAssigningSolutionModel` (converted to the engine shape,
    solved, and returned as a new model, the one passed in left as it was; needs pydantic).

    ``config`` becomes the base settings (`SolverConfig`; None = engine default
    30 s budget). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 (including 0 unassigned) is
    reached. ``on_new_best`` is called with ``(step, Score)`` on every new best.

    ``constraints`` picks the cost function directly
    (see `optimaru.task_assigning.constraints`). Only the listed constraints are scored; None
    uses all six default constraints at their default weights::

        from optimaru.task_assigning.constraints import get_constraints
        ta.solve(solution, constraints=get_constraints())
    """
    cfg = _build_config(
        time_limit_seconds=time_limit_seconds,
        random_seed=random_seed,
        parallel_solver_count=parallel_solver_count,
        stop_at_first_feasible=stop_at_first_feasible,
        full_assert=full_assert,
        on_new_best=on_new_best,
        config=config,
    )
    if isinstance(solution, TaskAssigningSolution):
        solve_in_place(Solver(cfg), solution, constraints)
        return solution

    # pydantic model path (lazy import: pydantic is an optional dependency).
    from .models import TaskAssigningSolutionModel

    if not isinstance(solution, TaskAssigningSolutionModel):
        raise TypeError(
            f"solution must be a TaskAssigningSolution or a models.TaskAssigningSolutionModel: {type(solution)!r}"
        )
    core_solution = solution.to_core()
    solve_in_place(Solver(cfg), core_solution, constraints)
    return TaskAssigningSolutionModel.from_core(core_solution)
