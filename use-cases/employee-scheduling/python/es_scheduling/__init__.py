"""Employee shift scheduling — C++ wrapper on the optimaru engine, with Python helpers.

`Employee`, `Shift`, `EmployeeSchedule`, `Score`, `Solver`, `SolverConfig`,
`calculate_score`, `constraint_costs`, `constraint_info` and `minutes` come from
the compiled pybind11 module (`_core`). The cost function is assembled from
constraint specs (see `constraints.py`, one class per constraint):
`DecomposedCalculator([RequiredSkillCost(), UndesiredDayCost(weight=2.0), ...])`
— or by name via `DecomposedCalculator().add("undesired_day_cost", weight=2.0)` /
`default_calculator()`. `constraint_info()` lists all 30 constraints with their
C++ default level and weight (the eight defaults first); `list_constraints()`
the names the rule factory accepts. `make_random_roster`, `summary` and
`to_dataframe` are pure-Python conveniences.

The JSON-friendly request/solution layer is `es_scheduling.models` (optional
pydantic dependency); `es_scheduling.demo.generate_demo_data` builds a sample
request and `with_initial_solution` restarts a solve from an earlier result.
"""

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

__all__ = [
    "Employee",
    "Shift",
    "EmployeeSchedule",
    "Score",
    "Solver",
    "SolverConfig",
    "DecomposedCalculator",
    "default_calculator",
    "CONSTRAINT_NAMES",
    "ConstraintInfo",
    "constraint_info",
    "constraint_costs",
    "list_constraints",
    "Constraint",
    "RequiredSkillCost",
    "EmployeeUnavailabilityCost",
    "ShiftOverlapCost",
    "MaxOneShiftPerDayCost",
    "MinRestCost",
    "UndesiredDayCost",
    "DesiredDayCost",
    "DesiredShiftCost",
    "ShiftCountImbalanceCost",
    "MaxConsecutiveDaysCost",
    "MinConsecutiveDaysCost",
    "MaxConsecutiveDaysOffCost",
    "RestAfterConsecutiveWorkCost",
    "MaxWeeklyMinutesCost",
    "MinWeeklyMinutesCost",
    "MinWeeklyRestDaysCost",
    "AssignmentCost",
    "UnassignedShiftsCost",
    "ShiftCountGapCost",
    "WeeklyMinutesGapCost",
    "ShiftCoverageCost",
    "LocationRunLengthCost",
    "RestAfterLocationRunCost",
    "LocationRunGapCost",
    "WorkerLocationCountCost",
    "DayPatternCost",
    "TargetDaysOffCost",
    "WorkerDayCountInRangeCost",
    "LocationImbalanceCost",
    "WorkerPairingCost",
    "calculate_score",
    "minutes",
    "MAX_DAY",
    "MINUTES_PER_DAY",
    "make_random_roster",
    "summary",
    "to_dataframe",
    "with_initial_solution",
]


def __getattr__(name):
    # Resolved on first use: pydantic is an optional dependency, and the C++
    # API above has to stay importable without it.
    if name == "with_initial_solution":
        from .models import with_initial_solution

        return with_initial_solution
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
