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
PACKAGE = "optimaru.maintenance_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""

#: A fixed Monday (epoch day) so instances are reproducible; the demo
#: generator otherwise starts from "next Monday".
FROM_DATE = 20_454


def _build(size: str) -> Any:
    import optimaru.maintenance_scheduling as mnt

    return mnt.generate_demo_data(size, FROM_DATE)


def instances() -> dict[str, Callable[[], Any]]:
    return {
        "small": lambda: _build("SMALL"),
        "large": lambda: _build("LARGE"),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.jobs)} jobs / {problem.crew_count()} crews / {problem.date_count()} dates"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import optimaru.maintenance_scheduling as mnt

    config = mnt.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    mnt.Solver(config).solve(problem)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.maintenance_scheduling import solve

    from optimaru.maintenance_scheduling.models import MaintenanceScheduleModel

    solved = solve(
        MaintenanceScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
