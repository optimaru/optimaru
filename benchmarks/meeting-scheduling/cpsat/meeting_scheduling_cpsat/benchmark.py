"""Engine vs CP-SAT comparison for meeting-scheduling.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`meeting_scheduling.constraint_costs`).
Run: `uv run python -m meeting_scheduling_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "meeting-scheduling"
PACKAGE = "meeting_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def instances() -> dict[str, Callable[[], Any]]:
    import meeting_scheduling.models

    return {"demo": lambda: meeting_scheduling.models.demo_data().to_core()}


def describe(problem: Any) -> str:
    return "Java quickstart demo (24 meetings)"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import meeting_scheduling.models

    meeting_scheduling.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from meeting_scheduling.models import MeetingScheduleModel

    from meeting_scheduling_cpsat import solve

    solved = solve(
        MeetingScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
