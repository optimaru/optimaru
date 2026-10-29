"""Maintenance scheduling — Python wrapper of the C++ solver on the optimaru engine.

A port of the `maintenance-scheduling` example model: assigns maintenance jobs (Job)
a crew and a start date (TimeGrain excluding weekends). The Java domain's two planning
variables (crew, startDate) are composite-encoded into one engine BASIC variable::

    crew_date = start_date_index * crew_count + crew_index   # -1 == unassigned

Dates are exchanged as epoch days (days since 1970-01-01, Java LocalDate.toEpochDay).
The pydantic mirror (`maintenance_scheduling.models`, optional dependency) converts to
and from `datetime.date`.

Constraints (HardSoftScore — same semantics as the Java ConstraintProvider):
  hard 1. crewConflict        -overlapping days per overlapping job pair on the same crew
  hard 2. minStartDate        -1 per day started before minStartDate
  hard 3. maxEndDate          -1 per day ended after maxEndDate
  soft 4. beforeIdealEndDate  -1 per day ended before idealEndDate
  soft 5. afterIdealEndDate   -1'000'000 per day ended after idealEndDate
  soft 6. tagConflict         -1'000 per shared tag x overlapping day of overlapping job pairs

Usage::

    import maintenance_scheduling as mnt

    schedule = mnt.generate_demo_data("SMALL")
    mnt.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(schedule.score, schedule.score.is_feasible())
"""

from ._core import (
    Crew,
    Job,
    MaintenanceSchedule,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    generate_demo_data,
    list_constraints,
    plus_business_days,
)

__all__ = [
    "Crew",
    "Job",
    "MaintenanceSchedule",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "generate_demo_data",
    "list_constraints",
    "constraint_costs",
    "plus_business_days",
    "solve",
]


def solve(
    schedule: MaintenanceSchedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> MaintenanceSchedule:
    """High-level API that solves a `MaintenanceSchedule` in place and returns it.

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS
    on). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -1e18)).

    ``constraints`` picks the cost function directly
    (see `maintenance_scheduling.constraints`). Only the listed constraints are scored;
    None uses all six default constraints::

        from maintenance_scheduling.constraints import get_constraints
        mnt.solve(schedule, constraints=get_constraints())
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
