"""Use-case-local engine/CP-SAT comparison on generated bin-packing instances.

The formulation, deterministic comparison instances, and CLI entry point live
together in this use-case package. Falkenauer's public instances run
through ``bench/run_bench.py`` (downloaded on demand); these instances need no
download, so CI can smoke-test both solvers.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "bin-packing"
PACKAGE = "bin_packing"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def _uniform(count: int, capacity: int, low: int, high: int, seed: int, colors: int = 0) -> Any:
    """Falkenauer-style 'u' instance: uniform weights; optionally every third item coloured
    and a few conflict pairs, so all four constraints are in play."""
    import bin_packing as bp

    rng = random.Random(seed)
    items = [
        bp.Item(
            f"I{index + 1:03d}",
            rng.randint(low, high),
            color=f"c{rng.randrange(colors)}" if colors and index % 3 == 0 else None,
        )
        for index in range(count)
    ]
    conflicts = []
    if colors:
        for _ in range(count // 10):
            first, second = rng.sample(range(count), 2)
            conflicts.append((items[first].id, items[second].id))
    return bp.BinPackingProblem(capacity, items, conflicts=conflicts)


def instances() -> dict[str, Callable[[], Any]]:
    return {
        "small": lambda: _uniform(12, 100, 20, 60, seed=0, colors=3),
        "medium": lambda: _uniform(30, 150, 20, 100, seed=1, colors=4),
        "large": lambda: _uniform(60, 150, 20, 100, seed=2),
    }


def describe(problem: Any) -> str:
    return f"{problem.item_count()} items / capacity {problem.bin_capacity}"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import bin_packing as bp

    bp.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from bin_packing.models import BinPackingProblemModel
    from external_solver.bin_packing import solve

    solved = solve(
        BinPackingProblemModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
