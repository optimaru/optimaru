"""Food packaging — Python wrapper of the C++ solver on the optimaru engine.

Assigns packaging jobs (Job) to production lines (Line) in order. The decision is
Line.jobs, an ordered list of jobs per line (the engine's sequence shape); jobs in no
line's list are unassigned, an extra unassigned pool list exposed to the engine.
Each job's times are derived from list positions::

    startCleaning(first job)  = line.start          # zero-length cleaning window
    startCleaning(otherwise)  = end of the previous job
    startProduction           = startCleaning + cleanup(previous product -> product)
    end                     = startProduction + duration

The second decision, each line's operator (Line.operator_index), is optimized by
alternation: the solver wrapper reassigns it by hill climbing between engine legs (the
engine supports one variable model per solution). With operators >= lines the assignment
is injective and the conflict constraint is always 0, so it is exactly equivalent to one
engine run.

Times are exchanged as "minutes" (int64) from a shared origin (the demo instance has
Monday 00:00 == minute 0; the pydantic mirror `optimaru.food_packaging.models` converts between
epoch minutes and `datetime.datetime`).

Constraints (three score levels, hard / medium / soft; the mapping onto engine Score is
the domain's medium fold engine.soft = medium * 10^10 + soft, and the order is
preserved exactly while |soft| < 10^10 and |medium| < 9*10^5):
  hard   1. max_end_time_cost                -1 per minute past max_end_time
  hard   2. operator_cleaning_conflict_cost  for job pairs on lines of the same operator
                                             whose cleaning windows [startCleaning,
                                             startProduction) overlap, -overlapping
                                             minutes
  medium 3. after_ideal_end_time_cost        -1 per minute past ideal_end_time
  medium 4. unassigned_jobs_cost             -duration minutes per unassigned job
  soft   5. squared_makespan_cost            -(end - line start)^2 per last job
                                             of a line
(min_start_minute and priority are data fields no constraint reads.)

The constraints and their defaults (level, weight in units of that level) live in the C++
components; `constraint_info()` reports them, `list_constraints()` lists the names and
`constraint_costs()` gives the raw per-constraint costs of a schedule (the only
per-constraint surface)::

    {row.name: (row.default_level, row.default_weight) for row in fp.constraint_info()}
    # {'max_end_time_cost': ('hard', 1.0), ..., 'unassigned_jobs_cost': ('medium', 1.0)}

Usage::

    import optimaru.food_packaging as fp

    schedule = fp.generate_demo_data()
    fp.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(schedule.score, schedule.score.is_feasible())
"""

import copy

from ._core import (
    ConstraintInfo,
    Job,
    Line,
    Operator,
    PackagingSchedule,
    Product,
    Score,
    Solver,
    SolverConfig,
    assign_missing_operators,
    calculate_score,
    constraint_costs,
    constraint_info,
    generate_demo_data,
    improve_operator_assignment,
    list_constraints,
)

__all__ = [
    "ConstraintInfo",
    "Job",
    "Line",
    "Operator",
    "PackagingSchedule",
    "Product",
    "Score",
    "Solver",
    "SolverConfig",
    "assign_missing_operators",
    "calculate_score",
    "generate_demo_data",
    "list_constraints",
    "constraint_costs",
    "constraint_info",
    "improve_operator_assignment",
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


def solve(
    schedule: PackagingSchedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> PackagingSchedule:
    """High-level API that solves a `PackagingSchedule` in place and returns it.

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS
    on). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -inf)).

    ``constraints`` picks the cost function directly
    (see `optimaru.food_packaging.constraints`). Only the listed constraints are scored; None
    uses all five default constraints::

        from optimaru.food_packaging.constraints import UnassignedJobsCost, get_constraints
        fp.solve(schedule, constraints=get_constraints())
        # unassigned minutes as soft instead of medium:
        others = [spec for spec in get_constraints() if spec.name != "unassigned_jobs_cost"]
        fp.solve(schedule, constraints=[*others, UnassignedJobsCost(level="soft")])

    The result's score then reports each term on the level the selection gave it.
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
    if constraints is None:
        Solver(cfg).solve(schedule)
    else:
        Solver(cfg).solve_with_constraints(schedule, constraints)
    return schedule
