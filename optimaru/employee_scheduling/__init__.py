"""Employee shift scheduling — C++ wrapper on the optimaru engine, with Python helpers.

`Employee`, `Shift`, `EmployeeSchedule`, `Score`, `Solver`, `SolverConfig`,
`calculate_score`, `constraint_costs`, `constraint_info` and `minutes` come from
the compiled pybind11 module (`_core`). The cost function is assembled from
constraint specs (see `constraints.py`, one class per constraint):
`DecomposedCalculator([RequiredSkillCost(), UndesiredDayCost(weight=2.0), ...])`
— or by name via `DecomposedCalculator().add("undesired_day_cost", weight=2.0)` /
`default_calculator()`. `constraint_info()` lists all 30 constraints with their
C++ default level and weight (the eight defaults first); `list_constraints()`
the names the rule factory accepts. `solve(schedule, ...)` solves a schedule in
place and returns it, and given a `models.ScheduleRequest` it returns a
`models.ScheduleSolution`; `make_random_roster`, `summary` and `to_dataframe` are
pure-Python conveniences.

The JSON-friendly request/solution layer is `optimaru.employee_scheduling.models` (pydantic);
`optimaru.employee_scheduling.demo.generate_demo_data` builds a sample request and
`with_initial_solution` restarts a solve from an earlier result.
"""

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
from .models import ScheduleRequest, ScheduleSolution, _solve_request, with_initial_solution

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
    "summary",
    "to_dataframe",
    "with_initial_solution",
]


def solve(
    schedule: EmployeeSchedule | ScheduleRequest,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> EmployeeSchedule | ScheduleSolution:
    """High-level API that solves `schedule` in place and returns it.

    Given a `models.ScheduleRequest` instead, the pydantic request (JSON in, JSON out),
    `solve` leaves it unchanged and returns a `models.ScheduleSolution`. The request
    carries its own settings and rules, and a keyword argument that is given replaces
    them: ``time_limit_seconds`` its ``timelimit``, ``random_seed`` and
    ``stop_at_first_feasible`` the fields of ``settings``, and ``parallel_solver_count``
    the number of starts (``num_workers``, with ``settings.multi_start`` set to 1).
    Starts beyond the first run in separate processes (see `models.solve_request`), so
    ``on_new_best`` needs a single start there. ``config`` and ``constraints`` are for
    an `EmployeeSchedule`: a request says the same in ``settings`` and
    ``model_input.rules``, and passing either with one raises ValueError.

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
    if isinstance(schedule, ScheduleRequest):
        if config is not None or constraints is not None:
            raise ValueError(
                "config and constraints are for an EmployeeSchedule: a ScheduleRequest carries them "
                "as settings and model_input.rules"
            )
        update: dict = {}
        settings: dict = {}
        if time_limit_seconds is not None:
            update["timelimit"] = time_limit_seconds
        if random_seed is not None:
            settings["random_seed"] = random_seed
        if stop_at_first_feasible:
            settings["stop_at_first_feasible"] = True
        if parallel_solver_count is not None:
            if parallel_solver_count < 1:
                raise ValueError(f"parallel_solver_count must be at least 1: {parallel_solver_count}")
            update["num_workers"] = parallel_solver_count
            settings["multi_start"] = 1
        if settings:
            update["settings"] = schedule.settings.model_copy(update=settings)
        request = schedule.model_copy(update=update) if update else schedule
        return _solve_request(request, full_assert=full_assert, on_new_best=on_new_best)
    if not isinstance(schedule, EmployeeSchedule):
        raise TypeError(f"schedule must be an EmployeeSchedule or a models.ScheduleRequest: {type(schedule)!r}")
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
