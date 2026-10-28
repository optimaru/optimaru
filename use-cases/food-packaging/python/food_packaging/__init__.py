"""Food packaging — Python wrapper of the C++ solver on the optimaru engine.

A port of the Timefold quickstart `food-packaging`: assigns packaging jobs (Job) to
production lines (Line) in order. The Java domain's Line.jobs
(@PlanningListVariable, allowsUnassignedValues=true) maps 1:1 onto the engine's LIST
variable, and "unassigned" is an extra unassigned pool list exposed to the engine.
Java's shadow variable chain is derived from list positions::

    startCleaning(first job)  = line.start          # zero-length cleaning window
    startCleaning(otherwise)  = end of the previous job
    startProduction           = startCleaning + cleanup(previous product -> product)
    end                     = startProduction + duration

Java's second planning variable, Line.operator, is optimized by alternation: the solver
wrapper reassigns it by hill climbing between engine legs (the engine supports one
variable model per solution). With operators >= lines the assignment is injective and
the conflict constraint is always 0, so it is exactly equivalent to one engine run.

Times are exchanged as "minutes" (int64) from a shared origin (the demo instance has
Monday 00:00 == minute 0; the pydantic mirror `food_packaging.models` converts between
epoch minutes and `datetime.datetime`).

Constraints (Java uses HardMediumSoftScore; the mapping onto engine HardSoftScore is
engine.soft = medium * 10^10 + soft, and the order is preserved exactly while
|soft| < 10^10 and |medium| < 9*10^5):
  hard   1. maxEndDateTime           -1 per minute past maxEndTime
  hard   2. operatorCleaningConflict for job pairs on lines of the same operator whose
                                     cleaning windows [startCleaning, startProduction)
                                     overlap, -overlapping minutes
  medium 3. idealEndDateTime         -1 per minute past idealEndTime
  medium 4. maximizeJobsAssigned     -duration minutes per unassigned job
  soft   5. minimizeMakespan         -(end - line start)^2 per last job
                                     of a line
(minStartTime and priority are data fields no constraint reads, the same as in
Java.)

Usage::

    import food_packaging as fp

    schedule = fp.generate_demo_data()
    fp.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(schedule.score, schedule.score.is_feasible())
"""

from ._core import (
    ConstraintBreakdown,
    Job,
    Line,
    Operator,
    PackagingSchedule,
    Product,
    Score,
    Solver,
    SolverConfig,
    analyze,
    assign_missing_operators,
    calculate_score,
    constraint_costs,
    generate_demo_data,
    improve_operator_assignment,
    list_constraints,
)

__all__ = [
    "ConstraintBreakdown",
    "Job",
    "Line",
    "Operator",
    "PackagingSchedule",
    "Product",
    "Score",
    "Solver",
    "SolverConfig",
    "analyze",
    "assign_missing_operators",
    "calculate_score",
    "generate_demo_data",
    "list_constraints",
    "constraint_costs",
    "improve_operator_assignment",
    "solve",
]


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
    (best_score_limit=(0, -1e18)).

    ``constraints`` picks the cost function directly
    (see `food_packaging.constraints`). Only the listed constraints are scored; None
    uses all five default constraints::

        from food_packaging.constraints import get_constraints
        fp.solve(schedule, constraints=get_constraints())
    """
    cfg = config if config is not None else SolverConfig()
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
    if constraints is None:
        Solver(cfg).solve(schedule)
    else:
        Solver(cfg).solve_with_constraints(schedule, constraints)
    return schedule
