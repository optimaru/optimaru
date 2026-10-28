"""Use-case-local engine/CP-SAT comparison.

The formulation, deterministic comparison instances, and CLI entry point live
together in this use-case package.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "maintenance-scheduling"
PACKAGE = "maintenance_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""

#: A fixed Monday (epoch day) so instances are reproducible; the demo
#: generator otherwise starts from "next Monday".
FROM_DATE = 20_454


def _build(size: str) -> Any:
    import maintenance_scheduling as mnt

    return mnt.generate_demo_data(size, FROM_DATE)


def instances() -> dict[str, Callable[[], Any]]:
    return {
        "small": lambda: _build("SMALL"),
        "large": lambda: _build("LARGE"),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.jobs)} jobs / {problem.crew_count()} crews / {problem.date_count()} dates"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import maintenance_scheduling as mnt

    config = mnt.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    mnt.Solver(config).solve(problem)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from maintenance_scheduling.models import MaintenanceScheduleModel

    from maintenance_scheduling_cpsat import solve

    solved = solve(
        MaintenanceScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()



# --- Timefold quickstart (benchmarks/timefold-quickstarts) ---------------------

TIMEFOLD = "maintenance-scheduling"


def _iso(epoch_day: int | None) -> str | None:
    import datetime

    return None if epoch_day is None else (datetime.date(1970, 1, 1) + datetime.timedelta(days=epoch_day)).isoformat()


def _epoch(iso: str) -> int:
    import datetime

    return (datetime.date.fromisoformat(iso) - datetime.date(1970, 1, 1)).days


def to_timefold(problem: Any) -> dict:
    jobs = []
    for index, job in enumerate(problem.jobs):
        assigned = job.crew_date >= 0
        jobs.append(
            {
                "id": job.id,
                "name": job.name,
                "durationInDays": job.duration_in_days,
                "minStartDate": _iso(job.min_start_date),
                "maxEndDate": _iso(job.max_end_date),
                "idealEndDate": _iso(job.ideal_end_date),
                "tags": list(job.tags),
                "crew": {"id": problem.crews[problem.crew_index_of(job.crew_date)].id} if assigned else None,
                "startDate": _iso(problem.job_start_date(index)) if assigned else None,
            }
        )
    return {
        "workCalendar": {"id": "calendar", "fromDate": _iso(problem.from_date), "toDate": _iso(problem.to_date)},
        "crews": [{"id": crew.id, "name": crew.name} for crew in problem.crews],
        "jobs": jobs,
    }


def from_timefold(problem: Any, result: dict) -> None:
    crew_index = {crew.id: i for i, crew in enumerate(problem.crews)}
    date_index = {day: i for i, day in enumerate(problem.start_dates())}
    chosen = result["assignment"]["Job"]
    assignment = []
    for job in problem.jobs:
        variables = chosen[job.id]
        if variables["crew"] is None or variables["startDate"] is None:
            assignment.append(-1)
        else:
            assignment.append(
                problem.crew_date_of(date_index[_epoch(variables["startDate"])], crew_index[variables["crew"]])
            )
    problem.set_assignment(assignment)


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
