"""Engine vs CP-SAT comparison for knapsack.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`knapsack.constraint_costs`).
Run: `uv run python -m knapsack_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli, seeded

NAME = "knapsack"
PACKAGE = "optimaru.knapsack"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> tuple[float, ...]:
    # The engine's own score of its answer (the score fields solve_engine copies
    # from the solved model), cross-checked against the rescored one.
    return (problem.score_hard, problem.score_soft)


def _build(item_count: int, seed: int) -> Any:
    from optimaru.knapsack.models import ItemModel, KnapsackProblemModel

    rng = random.Random(seed)
    items = [ItemModel(id=f"I{i}", weight=rng.randint(5, 60), value=rng.randint(10, 100)) for i in range(item_count)]
    return KnapsackProblemModel(capacity=sum(item.weight for item in items) // 3, items=items)


def instances() -> dict[str, Callable[[], Any]]:
    # The first seed of a scale is the instance of that name; the others are `<scale>-2`, `<scale>-3`, ...
    return seeded(
        {
            "n50": (lambda seed: _build(50, seed=seed), (0, 7, 8)),
            "n100": (lambda seed: _build(100, seed=seed), (3, 9, 10)),
            "n200": (lambda seed: _build(200, seed=seed), (1, 11, 12)),
            "n500": (lambda seed: _build(500, seed=seed), (4, 13, 14)),
            "n1000": (lambda seed: _build(1000, seed=seed), (2, 15, 16)),
            "n2000": (lambda seed: _build(2000, seed=seed), (5, 17, 18)),
            "n5000": (lambda seed: _build(5000, seed=seed), (6, 19)),
        }
    )


def describe(problem: Any) -> str:
    return f"{len(problem.items)} items"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    from optimaru import knapsack

    solved = knapsack.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)
    # knapsack.solve returns a new model; the runner scores the one it handed in.
    for name in type(solved).model_fields:
        setattr(problem, name, getattr(solved, name))


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.knapsack import solve

    return solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=workers)


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
