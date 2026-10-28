"""Engine vs CP-SAT comparison for knapsack.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`knapsack.constraint_costs`).
Run: `uv run python -m knapsack_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "knapsack"
PACKAGE = "knapsack"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None

import random

def _build(item_count: int, seed: int) -> Any:
    from knapsack.models import ItemModel, KnapsackProblemModel

    rng = random.Random(seed)
    items = [
        ItemModel(id=f"I{i}", weight=rng.randint(5, 60), value=rng.randint(10, 100)) for i in range(item_count)
    ]
    return KnapsackProblemModel(capacity=sum(item.weight for item in items) // 3, items=items)


def instances() -> dict[str, Callable[[], Any]]:
    return {"n50": lambda: _build(50, seed=0), "n200": lambda: _build(200, seed=1), "n1000": lambda: _build(1000, seed=2)}


def describe(problem: Any) -> str:
    return f"{len(problem.items)} items"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import knapsack

    knapsack.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from knapsack_cpsat import solve

    return solve(problem, time_limit_seconds=seconds, random_seed=seed, num_workers=workers)


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
