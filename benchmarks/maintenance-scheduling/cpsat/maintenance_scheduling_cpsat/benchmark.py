"""Use-case-local engine/CP-SAT comparison.

The formulation, deterministic comparison instances, and CLI entry point live
together in this use-case package.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from datetime import date, timedelta
from typing import Any

from cpsat_plugins.benchmark import run_cli, seeded

NAME = "maintenance-scheduling"
PACKAGE = "optimaru.maintenance_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""

#: A fixed Monday (epoch day) so instances are reproducible; the demo
#: generator otherwise starts from "next Monday".
FROM_DATE = 20_454


AREAS = ["Downtown", "Uptown", "Park", "Airport", "Bay", "Hill", "Subway", "Harbor", "Campus", "Station"]
WORKS = ["Street", "Bridge", "Tunnel", "Highway", "Boulevard", "Avenue", "Square", "Plaza"]
CREWS = ["Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta", "Theta", "Iota", "Kappa"]


def _build(size: str) -> Any:
    import optimaru.maintenance_scheduling as mnt

    return mnt.generate_demo_data(size, FROM_DATE)


def _generate(job_count: int, crew_count: int, weeks: int, seed: int) -> Any:
    """A seeded instance in the shape of the demo data: jobs of 1 to 10 working days, each
    with a window that fits it with slack and an ideal end a few days before the window
    closes, tagged with its area (jobs of one area should not overlap)."""
    from optimaru.maintenance_scheduling.models import CrewModel, JobModel, MaintenanceScheduleModel

    rng = random.Random(seed)
    start = date.fromordinal(date(1970, 1, 1).toordinal() + FROM_DATE)
    days = weeks * 7
    jobs = []
    for index in range(job_count):
        area = AREAS[(index // len(WORKS)) % len(AREAS)]
        duration = rng.randint(1, 10)
        # Calendar days the job needs at five working days a week, plus slack.
        window = min(days, duration * 7 // 5 + rng.randint(8, 42))
        min_start = start + timedelta(days=rng.randint(0, days - window))
        max_end = min_start + timedelta(days=window)
        jobs.append(
            JobModel(
                id=str(index),
                name=f"{area} {WORKS[index % len(WORKS)]} {index // (len(WORKS) * len(AREAS)) + 1}",
                duration_in_days=duration,
                min_start_date=min_start,
                max_end_date=max_end,
                ideal_end_date=max_end - timedelta(days=rng.randint(1, 6)),
                tags=[area],
            )
        )
    return MaintenanceScheduleModel(
        from_date=start,
        to_date=start + timedelta(days=days),
        crews=[CrewModel(id=str(index + 1), name=f"{CREWS[index]} crew") for index in range(crew_count)],
        jobs=jobs,
    ).to_core()


def instances() -> dict[str, Callable[[], Any]]:
    # The first seed of a scale is the instance of that name; the others are `<scale>-2`, `<scale>-3`, ...
    return seeded(
        {
            # The demo data's two sizes are `small` and `large`; the other instances of
            # those scales are generated in their shape.
            "small": (lambda seed: _build("SMALL") if seed is None else _generate(14, 3, 8, seed=seed), (None, 4, 5, 6, 7, 8)),
            "medium": (lambda seed: _generate(28, 4, 12, seed=seed), (1, 2, 9, 10, 11, 12)),
            "large": (lambda seed: _build("LARGE") if seed is None else _generate(48, 5, 16, seed=seed), (None, 13, 14, 15, 16)),
            "xlarge": (lambda seed: _generate(96, 8, 24, seed=seed), (3, 17, 18)),
        }
    )


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
        parallel_solver_count=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
