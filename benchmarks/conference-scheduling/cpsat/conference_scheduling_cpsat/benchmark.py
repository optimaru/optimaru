"""Engine vs CP-SAT comparison for conference-scheduling.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`conference_scheduling.constraint_costs`).
Run: `uv run python -m conference_scheduling_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "conference-scheduling"
PACKAGE = "conference_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def instances() -> dict[str, Callable[[], Any]]:
    import conference_scheduling

    return {"demo": lambda: conference_scheduling.demo_conference()}


def describe(problem: Any) -> str:
    return "Java quickstart demo conference"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import conference_scheduling

    conference_scheduling.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from conference_scheduling.models import ConferenceScheduleModel

    from conference_scheduling_cpsat import solve

    solved = solve(
        ConferenceScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
