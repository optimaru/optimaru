"""Engine vs CP-SAT comparison for task-assigning.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`task_assigning.constraint_costs`).
Run: `uv run python -m task_assigning_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "task-assigning"
PACKAGE = "optimaru.task_assigning"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def instances() -> dict[str, Callable[[], Any]]:
    from optimaru.task_assigning import demo

    return {"demo": lambda: demo.generate_demo_data().to_core()}


def describe(problem: Any) -> str:
    return "quickstart-scale demo"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    from optimaru import task_assigning

    task_assigning.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from optimaru.task_assigning.models import TaskAssigningSolutionModel

    from external_solver.task_assigning import solve

    solved = solve(
        TaskAssigningSolutionModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
