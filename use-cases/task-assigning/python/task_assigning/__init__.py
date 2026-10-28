"""Task assigning — Python wrapper of the C++ solver on the optimaru engine.

`Customer`/`TaskType`/`Task`/`Employee`/`TaskAssigningSolution`/`Score`/
`Solver`/`SolverConfig` and `calculate_score` come from the compiled C++ core (`_core`).
The planning variable is each `Employee.tasks` — the ordered list of task indices the
employee performs back to back (the Java original's
`@PlanningListVariable(allowsUnassignedValues = true)`). A task in no list is
unassigned.

Shadow variables are derived by the same rules as the Java original
(recomputed by `calculate_score`/`Solver.solve`):

    task.employee    == index of the employee owning the list (-1 == unassigned)
    task.start_time  == the first task gets its own min_start_time,
                        later tasks max(end of the previous task, min_start_time)
    duration         == base_duration x affinity multiplier (HIGH 1 / MEDIUM 2 /
                        LOW 3 / NONE 4)

Score convention (BendableScore [1]hard/[3]soft, as in the Java quickstart; all penalties):
  hard[0]  noMissingSkills              required skills the employee lacks
  soft[0]  minimizeUnassignedTasks      1 per unassigned task
  soft[1]  minimizeMakespan             endTime^2 per employee
  soft[2]  {critical|major|minor}PriorityTaskEndTime
                                        endTime x 4 / x 2 / x 1
Internally it maps onto HardSoftScore(int64, double) as
`engine.hard = hard * 10**9 + soft0`, `engine.soft = soft1 * 10**7 + soft2`
(level order preserved; see cpp/include/ta/domain.hpp for the exact rules).

The high-level entry point is `solve`::

    import task_assigning as ta
    from task_assigning.demo import generate_demo_data

    solution = generate_demo_data()      # models.TaskAssigningSolutionModel
    result = ta.solve(solution, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_soft0, result.score_soft1, result.score_soft2)

The pydantic mirror (for serialization) is in `task_assigning.models` (optional dependency).
"""

from ._core import (
    Affinity,
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
    list_constraints,
)

__all__ = [
    "Affinity",
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
    "solve",
    "list_constraints",
    "constraint_costs",
]


def _build_config(
    *,
    time_limit_seconds: float | None,
    random_seed: int | None,
    parallel_solver_count: int | None,
    stop_at_first_feasible: bool,
    full_assert: bool,
    on_new_best,
    config,
) -> SolverConfig:
    cfg = config if config is not None else SolverConfig()
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    if stop_at_first_feasible:
        # Stop as soon as hard 0 + every task assigned is reached (soft1/soft2 have an
        # effectively -inf floor).
        cfg.best_score_limit = (0, 0, -(2**62), -(2**62))
    if full_assert:
        cfg.full_assert = True
    if on_new_best is not None:
        cfg.on_new_best = on_new_best
    return cfg


def solve(
    solution,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config=None,
    constraints=None,
):
    """High-level API that solves a task assigning problem and returns it in the same shape.

    ``solution`` is a `_core.TaskAssigningSolution` (solved in place and
    returned) or a `models.TaskAssigningSolutionModel` (converted to the engine shape,
    solved, and returned as a new `TaskAssigningSolutionModel`; needs pydantic).

    ``config`` becomes the base settings (`SolverConfig`; None = engine default
    30 s budget). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 (including 0 unassigned) is
    reached. ``on_new_best`` is called with ``(step, Score)`` on every new best.

    ``constraints`` picks the cost function directly
    (see `task_assigning.constraints`). Only the listed constraints are scored; None
    uses all six default constraints::

        from task_assigning.constraints import get_constraints
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

    def _solve(core_solution):
        if constraints is None:
            Solver(cfg).solve(core_solution)
        else:
            Solver(cfg).solve_with_constraints(core_solution, constraints)

    if isinstance(solution, TaskAssigningSolution):
        _solve(solution)
        return solution

    # pydantic model path (lazy import: pydantic is an optional dependency).
    from .models import TaskAssigningSolutionModel

    if not isinstance(solution, TaskAssigningSolutionModel):
        raise TypeError(
            f"solution must be a TaskAssigningSolution or a models.TaskAssigningSolutionModel: {type(solution)!r}"
        )
    core_solution = solution.to_core()
    _solve(core_solution)
    return TaskAssigningSolutionModel.from_core(core_solution)
