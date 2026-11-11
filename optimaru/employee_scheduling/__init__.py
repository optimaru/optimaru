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

from typing import overload

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
from .models import (
    ScheduleRequest,
    ScheduleSolution,
    SchedulingInput,
    _request_with,
    _solve_model,
    _solve_request,
    with_initial_solution,
)

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


@overload
def solve(
    schedule: EmployeeSchedule,
    *,
    time_limit_seconds: float | None = ...,
    random_seed: int | None = ...,
    parallel_solver_count: int | None = ...,
    stop_at_first_feasible: bool = ...,
    full_assert: bool = ...,
    on_new_best=...,
    config: SolverConfig | None = ...,
    constraints=...,
) -> EmployeeSchedule: ...
@overload
def solve(
    schedule: ScheduleRequest,
    *,
    time_limit_seconds: float | None = ...,
    random_seed: int | None = ...,
    parallel_solver_count: int | None = ...,
    stop_at_first_feasible: bool = ...,
    full_assert: bool = ...,
    on_new_best=...,
    config: SolverConfig | None = ...,
    constraints=...,
) -> ScheduleSolution: ...
@overload
def solve(
    schedule: SchedulingInput,
    *,
    time_limit_seconds: float | None = ...,
    random_seed: int | None = ...,
    parallel_solver_count: int | None = ...,
    stop_at_first_feasible: bool = ...,
    full_assert: bool = ...,
    on_new_best=...,
    config: SolverConfig | None = ...,
    constraints=...,
) -> SchedulingInput: ...
def solve(
    schedule: EmployeeSchedule | SchedulingInput | ScheduleRequest,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> EmployeeSchedule | SchedulingInput | ScheduleSolution:
    """High-level API that solves a schedule and returns it in the same shape.

    `schedule` is an `EmployeeSchedule` (solved in place and returned) or a
    `models.SchedulingInput`, the pydantic problem (converted to the engine shape,
    solved, and returned as a new `SchedulingInput` with each shift's ``worker`` and
    the result fields filled in, the one passed in left as it was). A `SchedulingInput`
    is scored on its own ``rules``; ``constraints`` replaces them for the solve.

    A `models.ScheduleRequest`, the earlier pydantic shape that wraps a
    `SchedulingInput` with its time limit and settings, is still taken: `solve` leaves
    it unchanged and returns a `models.ScheduleSolution`. The request
    carries its own settings and rules, and a keyword argument that is given replaces
    them: ``time_limit_seconds`` its ``timelimit``, ``random_seed`` the seed in
    ``settings``, and ``parallel_solver_count`` the number of starts (``num_workers``,
    with ``settings.multi_start`` set to 1). ``stop_at_first_feasible`` turns the flag in
    ``settings`` on.
    Starts beyond the first run in separate processes (see `models.solve_request`), so
    ``on_new_best`` needs a single start there. ``config`` and ``constraints`` are for
    an `EmployeeSchedule` or a `SchedulingInput`: a request says the same in ``settings`` and
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

    The score is on the result: ``schedule.score``, or ``score_hard`` / ``score_soft`` on a model.
    """
    if isinstance(schedule, ScheduleRequest):
        if config is not None or constraints is not None:
            raise ValueError(
                "config and constraints are for an EmployeeSchedule or a SchedulingInput: a ScheduleRequest carries them "
                "as settings and model_input.rules"
            )
        request = _request_with(
            schedule,
            time_limit_seconds=time_limit_seconds,
            random_seed=random_seed,
            parallel_solver_count=parallel_solver_count,
            stop_at_first_feasible=stop_at_first_feasible,
        )
        return _solve_request(request, full_assert=full_assert, on_new_best=on_new_best)
    if not isinstance(schedule, (EmployeeSchedule, SchedulingInput)):
        raise TypeError(f"schedule must be an EmployeeSchedule or a models.SchedulingInput: {type(schedule)!r}")
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
    if isinstance(schedule, SchedulingInput):
        return _solve_model(schedule, cfg, constraints)
    if constraints is None:
        Solver(cfg).solve(schedule)
    else:
        calculator = constraints if isinstance(constraints, DecomposedCalculator) else DecomposedCalculator(constraints)
        Solver(cfg).solve(schedule, calculator)
    return schedule
