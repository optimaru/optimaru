"""Engine vs CP-SAT comparison for max-cut.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`max_cut.constraint_costs`).
Run: `uv run python -m max_cut_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "max-cut"
PACKAGE = "max_cut"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None

import random

def _build(vertex_count: int, density: float, seed: int) -> Any:
    from max_cut.models import EdgeModel, MaxCutProblemModel

    rng = random.Random(seed)
    vertices = [f"V{v}" for v in range(vertex_count)]
    edges = [
        EdgeModel(source=vertices[a], target=vertices[b], weight=rng.randint(1, 10))
        for a in range(vertex_count)
        for b in range(a + 1, vertex_count)
        if rng.random() < density
    ]
    return MaxCutProblemModel(vertices=vertices, edges=edges)


def instances() -> dict[str, Callable[[], Any]]:
    return {"g30": lambda: _build(30, 0.3, seed=0), "g80": lambda: _build(80, 0.1, seed=1), "g200": lambda: _build(200, 0.05, seed=2)}


def describe(problem: Any) -> str:
    return f"{len(problem.vertices)} vertices / {len(problem.edges)} edges"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import max_cut

    max_cut.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from max_cut_cpsat import solve

    return solve(problem, time_limit_seconds=seconds, random_seed=seed, num_workers=workers)


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
