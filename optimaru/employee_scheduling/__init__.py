"""Employee shift scheduling — C++ wrapper on the optimaru engine, with Python helpers.

`Employee`, `Shift`, `EmployeeSchedule`, `Score`, `Solver`, `SolverConfig`,
`calculate_score`, `constraint_costs`, `constraint_info` and `minutes` come from
the compiled pybind11 module (`_core`). The cost function is assembled from
constraint specs (see `constraints.py`, one class per constraint):
`DecomposedCalculator([RequiredSkillCost(), UndesiredDayCost(weight=2.0), ...])`
— or by name via `DecomposedCalculator().add("undesired_day_cost", weight=2.0)` /
`default_calculator()`. `constraint_info()` lists all 30 constraints with their
C++ default level and weight (the eight defaults first); `list_constraints()`
the names the constraint factory accepts. `solve(schedule, ...)` solves a schedule in
place and returns it; `make_random_roster`, `summary` and `to_dataframe` are
pure-Python conveniences.

The JSON-friendly request/solution layer is `optimaru.employee_scheduling.models` (pydantic);
`optimaru.employee_scheduling.demo.generate_demo_data` builds a sample request and
`with_initial_solution` restarts a solve from an earlier result.
"""

from .. import _api
from .._solve import solver_config
from ._core import (
    CONSTRAINT_NAMES,
    MAX_DAY,
    MINUTES_PER_DAY,
    ConstraintInfo,
    DecomposedCalculator,
    Employee,
    EmployeeSchedule,
    Score,
    Shift,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    constraint_info,
    default_calculator,
    minutes,
)
from .constraints import (
    AssignmentCost,
    Constraint,
    DayPatternCost,
    DesiredDayCost,
    DesiredShiftCost,
    EmployeeUnavailabilityCost,
    LocationImbalanceCost,
    LocationRunGapCost,
    LocationRunLengthCost,
    MaxConsecutiveDaysCost,
    MaxConsecutiveDaysOffCost,
    MaxOneShiftPerDayCost,
    MaxWeeklyMinutesCost,
    MinConsecutiveDaysCost,
    MinRestCost,
    MinWeeklyMinutesCost,
    MinWeeklyRestDaysCost,
    RequiredSkillCost,
    RestAfterConsecutiveWorkCost,
    RestAfterLocationRunCost,
    ShiftCountGapCost,
    ShiftCountImbalanceCost,
    ShiftCoverageCost,
    ShiftOverlapCost,
    TargetDaysOffCost,
    UnassignedShiftsCost,
    UndesiredDayCost,
    WeeklyMinutesGapCost,
    WorkerDayCountInRangeCost,
    WorkerLocationCountCost,
    WorkerPairingCost,
)
from .helpers import list_constraints, make_random_roster, summary, to_dataframe
from .models import solve_request, with_initial_solution

__all__ = [
    "CONSTRAINT_NAMES",
    "MAX_DAY",
    "MINUTES_PER_DAY",
    "AssignmentCost",
    "Constraint",
    "ConstraintInfo",
    "DayPatternCost",
    "DecomposedCalculator",
    "DesiredDayCost",
    "DesiredShiftCost",
    "Employee",
    "EmployeeSchedule",
    "EmployeeUnavailabilityCost",
    "LocationImbalanceCost",
    "LocationRunGapCost",
    "LocationRunLengthCost",
    "MaxConsecutiveDaysCost",
    "MaxConsecutiveDaysOffCost",
    "MaxOneShiftPerDayCost",
    "MaxWeeklyMinutesCost",
    "MinConsecutiveDaysCost",
    "MinRestCost",
    "MinWeeklyMinutesCost",
    "MinWeeklyRestDaysCost",
    "RequiredSkillCost",
    "RestAfterConsecutiveWorkCost",
    "RestAfterLocationRunCost",
    "Score",
    "Shift",
    "ShiftCountGapCost",
    "ShiftCountImbalanceCost",
    "ShiftCoverageCost",
    "ShiftOverlapCost",
    "Solver",
    "SolverConfig",
    "TargetDaysOffCost",
    "UnassignedShiftsCost",
    "UndesiredDayCost",
    "WeeklyMinutesGapCost",
    "WorkerDayCountInRangeCost",
    "WorkerLocationCountCost",
    "WorkerPairingCost",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "default_calculator",
    "list_constraints",
    "make_random_roster",
    "minutes",
    "solve",
    "solve_request",
    "summary",
    "to_dataframe",
    "with_initial_solution",
]


@_api.compiled_solve
def solve(
    schedule: EmployeeSchedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> EmployeeSchedule:
    """High-level API that solves `schedule` in place and returns it.

    ``config`` becomes the base settings (`SolverConfig`; None = engine defaults,
    a 30 s budget). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached, ``full_assert``
    re-verifies every incremental score against a full recompute (a debugging aid,
    much slower), and ``on_new_best(step, score)`` is called on every new best.

    ``constraints`` picks the cost function directly: a list of constraint specs
    (see `optimaru.employee_scheduling.constraints`) or a `DecomposedCalculator`.
    Only the listed constraints are scored, by a single search
    (``parallel_solver_count`` is ignored there); None uses the eight default
    constraints at their default weights::

        from optimaru.employee_scheduling.constraints import get_constraints
        es.solve(schedule, constraints=get_constraints())

    The score is on the result: ``schedule.score``.
    """
    if not isinstance(schedule, EmployeeSchedule):
        raise TypeError(
            f"schedule must be an instance of EmployeeSchedule: {type(schedule)!r} (models.solve_request solves a models.ScheduleRequest)"
        )
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
    if constraints is None:
        Solver(cfg).solve(schedule)
    else:
        calculator = constraints if isinstance(constraints, DecomposedCalculator) else DecomposedCalculator(constraints)
        Solver(cfg).solve(schedule, calculator)
    return schedule
