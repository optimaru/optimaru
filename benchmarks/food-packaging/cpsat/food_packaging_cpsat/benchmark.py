"""Engine vs CP-SAT comparison for food-packaging.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`food_packaging.constraint_costs`).
Run: `uv run python -m food_packaging_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "food-packaging"
PACKAGE = "optimaru.food_packaging"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def _plant(line_count: int, job_count: int) -> Callable[[], Any]:
    """The demo generator (deterministic, seed 37) at another scale."""

    def build() -> Any:
        from optimaru import food_packaging

        return food_packaging.generate_demo_data(line_count, job_count)

    return build


def instances() -> dict[str, Callable[[], Any]]:
    # The demo generator has one seed, so every instance is another scale: `linesNxM` is N lines
    # and M jobs beside the six plants named by their lines alone.
    return {
        "lines2x10": _plant(2, 10),
        "lines2": _plant(2, 20),
        "lines2x30": _plant(2, 30),
        "lines3x20": _plant(3, 20),
        "lines3x30": _plant(3, 30),
        "lines3": _plant(3, 40),
        "lines3x60": _plant(3, 60),
        "lines4x40": _plant(4, 40),
        "lines4": _plant(4, 60),
        "lines4x80": _plant(4, 80),
        "lines5x50": _plant(5, 50),
        "lines5x75": _plant(5, 75),
        # The demo plant: `generate_demo_data()` with its own defaults.
        "demo": _plant(5, 100),
        "lines6x90": _plant(6, 90),
        "lines6x120": _plant(6, 120),
        "lines7x140": _plant(7, 140),
        "lines8x150": _plant(8, 150),
        "lines8": _plant(8, 200),
        "lines9x250": _plant(9, 250),
        "lines10": _plant(10, 300),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.lines)} lines / {len(problem.jobs)} jobs"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    from optimaru import food_packaging

    food_packaging.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.food_packaging import solve

    from optimaru.food_packaging.models import PackagingScheduleModel

    solved = solve(
        PackagingScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        parallel_solver_count=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
