"""Employee shift scheduling — C++ wrapper on the optimaru engine, with Python helpers.

`Employee`, `Shift`, `EmployeeSchedule`, `Score`, `Solver`, `SolverConfig`,
`calculate_score` and `minutes` come from the compiled pybind11 module (`_core`).
The cost function is assembled from constraint specs (see `constraints.py`):
`DecomposedCalculator([RequiredSkill(), UndesiredDay(2.0), ...])` — or by
name via `DecomposedCalculator().add(...)` / `default_calculator()`.
`make_random_roster`, `summary` and `to_dataframe` are pure-Python conveniences.

The JSON-friendly request/solution layer is `es_scheduling.models` (optional
pydantic dependency); `es_scheduling.demo.generate_demo_data` builds a sample
request and `with_initial_solution` restarts a solve from an earlier result.
"""

from ._core import (
    CONSTRAINT_NAMES,
    MINUTES_PER_DAY,
    DecomposedCalculator,
    Employee,
    EmployeeSchedule,
    Score,
    Shift,
    Solver,
    SolverConfig,
    calculate_score,
    default_calculator,
    minutes,
)
from .constraints import (
    AssignmentCost,
    BalanceMinMaxGap,
    BalanceShiftAssignments,
    BalanceWeeklyMinutesGap,
    Constraint,
    DesiredDay,
    DesiredShift,
    MaxConsecutiveDays,
    MaxConsecutiveOff,
    MaxOneShiftPerDay,
    MaxWeeklyMinutes,
    MinConsecutiveDays,
    MinimumRest,
    MinWeeklyMinutes,
    MinWeeklyRestDays,
    OverlappingShift,
    RequiredSkill,
    RestAfterConsecutiveWork,
    UnassignedShift,
    UnavailableEmployee,
    UndesiredDay,
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
    "list_constraints",
    "Constraint",
    "RequiredSkill",
    "UnavailableEmployee",
    "OverlappingShift",
    "MaxOneShiftPerDay",
    "MinimumRest",
    "UndesiredDay",
    "DesiredDay",
    "DesiredShift",
    "BalanceShiftAssignments",
    "MaxConsecutiveDays",
    "MinConsecutiveDays",
    "MaxConsecutiveOff",
    "RestAfterConsecutiveWork",
    "MaxWeeklyMinutes",
    "MinWeeklyMinutes",
    "MinWeeklyRestDays",
    "AssignmentCost",
    "UnassignedShift",
    "BalanceMinMaxGap",
    "BalanceWeeklyMinutesGap",
    "calculate_score",
    "minutes",
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
