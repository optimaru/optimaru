"""Maintenance scheduling — Python wrapper of the C++ solver on the optimaru engine.

Assigns maintenance jobs (Job) a crew and a start date (weekdays only). The two
decisions (crew, start date) are composite-encoded into one engine assignment-shape variable::

    crew_date = start_date_index * crew_count + crew_index   # -1 == unassigned

Dates are exchanged as epoch days (days since 1970-01-01).
The pydantic mirror (`optimaru.maintenance_scheduling.models`) converts to and from `datetime.date`.

Constraints, at the default weights:
  hard 1. crew_conflict_cost          -overlapping days per overlapping job pair on the same crew
  hard 2. min_start_date_cost         -1 per day started before min_start_date
  hard 3. max_end_date_cost           -1 per day ended after max_end_date
  soft 4. before_ideal_end_date_cost  -1 per day ended before ideal_end_date
  soft 5. after_ideal_end_date_cost   -1'000'000 per day ended after ideal_end_date
  soft 6. tag_conflict_cost           -1'000 per shared tag x overlapping day of overlapping job pairs
Change a weight or a level through a constraint selection, e.g.
``solve(schedule, constraints=[..., TagConflictCost(weight=10_000)])``
(see `optimaru.maintenance_scheduling.constraints`). `constraint_info()` lists every constraint with
its default level and weight.

Usage::

    import optimaru.maintenance_scheduling as mnt

    schedule = mnt.generate_demo_data("SMALL")
    mnt.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(schedule.score, schedule.score.is_feasible())
"""

from typing import TypeVar

from .._solve import solve_in_place, solver_config
from ._core import (
    ConstraintInfo,
    Crew,
    Job,
    MaintenanceSchedule,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    constraint_info,
    generate_demo_data,
    list_constraints,
    plus_business_days,
)
from .models import MaintenanceScheduleModel, with_initial_solution

__all__ = [
    "ConstraintInfo",
    "Crew",
    "Job",
    "MaintenanceSchedule",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "generate_demo_data",
    "list_constraints",
    "plus_business_days",
    "solve",
    "with_initial_solution",
]


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


def solve(
    schedule: _Problem,
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
    """High-level API that solves a `MaintenanceSchedule` in place and returns it.

    `schedule` is a `MaintenanceSchedule` (solved in place and returned) or a
    `models.MaintenanceScheduleModel` (converted, solved, and returned as a new model; the
    one passed in is left as it was).

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS
    on). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -inf)).

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly
    (see `optimaru.maintenance_scheduling.constraints`). Only the listed constraints are scored,
    a name picked several times once per pick; None uses all six default constraints at
    their default weights::

        from optimaru.maintenance_scheduling.constraints import get_constraints
        mnt.solve(schedule, constraints=get_constraints())
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
    if isinstance(schedule, MaintenanceSchedule):
        solve_in_place(Solver(cfg), schedule, constraints)
        return schedule
    if isinstance(schedule, MaintenanceScheduleModel):
        core = schedule.to_core()
        solve_in_place(Solver(cfg), core, constraints)
        return MaintenanceScheduleModel.from_core(core)
    raise TypeError(f"schedule must be a MaintenanceSchedule or a models.MaintenanceScheduleModel: {type(schedule)!r}")
