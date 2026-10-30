"""Pure-Python conveniences on top of the C++ core (no C++ changes)."""

import random

from ._core import CONSTRAINT_NAMES, Employee, EmployeeSchedule, Shift, minutes


def list_constraints():
    """Constraint names available to `DecomposedCalculator.add`."""
    return list(CONSTRAINT_NAMES)


def make_random_roster(days=5, shifts_per_day=4, employees=6, skills=("Skill A", "Skill B", "Skill C"), seed=0):
    """Build a random (unassigned) EmployeeSchedule for demos and testing.

    Each employee gets a random subset of `skills` plus a few unavailable /
    desired days; each shift requires one skill. The solver assigns employees.
    """
    rng = random.Random(seed)
    emps = []
    for e in range(employees):
        subset = {s for s in skills if rng.random() < 0.6} or {rng.choice(skills)}
        emps.append(
            Employee(
                name=f"Employee {e}",
                skills=subset,
                unavailable_dates={d for d in range(days) if rng.random() < 0.1},
                desired_dates={d for d in range(days) if rng.random() < 0.15},
            )
        )
    shifts = []
    sid = 0
    for d in range(days):
        for k in range(shifts_per_day):
            start = minutes(d, 7 if k % 2 == 0 else 15, 0)
            shifts.append(Shift(f"Shift {sid}", start, start + 8 * 60, "Location A", skills[sid % len(skills)]))
            sid += 1
    return EmployeeSchedule(emps, shifts)


def summary(schedule):
    """Human-readable summary: score plus shift count per employee."""
    load = [0] * len(schedule.employees)
    for shift in schedule.shifts:
        if shift.employee_index >= 0:
            load[shift.employee_index] += 1
    lines = [f"score: {schedule.score}  feasible: {schedule.score.is_feasible()}"]
    for emp, n in zip(schedule.employees, load):
        lines.append(f"  {emp.name}: {n} shifts")
    return "\n".join(lines)


def to_dataframe(schedule):
    """Return the roster as a pandas DataFrame (one row per shift)."""
    try:
        import pandas as pd
    except ImportError as exc:  # pragma: no cover
        raise ImportError("to_dataframe needs pandas: pip install pandas") from exc

    employees = schedule.employees
    rows = [
        {
            "shift": s.id,
            "start_min": s.start,
            "end_min": s.end,
            "day": s.start // 1440,
            "skill": s.required_skill,
            "employee": employees[s.employee_index].name if s.employee_index >= 0 else None,
        }
        for s in schedule.shifts
    ]
    return pd.DataFrame(rows)
