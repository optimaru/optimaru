"""Engine vs CP-SAT comparison for max-cut.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`max_cut.constraint_costs`).
Run: `uv run python -m max_cut_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "max-cut"
PACKAGE = "optimaru.max_cut"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> tuple[float, ...]:
    # The engine's own score of its answer (the score fields solve_engine copies
    # from the solved model), cross-checked against the rescored one.
    return (problem.score_hard, problem.score_soft)


def _build(vertex_count: int, density: float, seed: int) -> Any:
    from optimaru.max_cut.models import EdgeModel, MaxCutProblemModel

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
    return {
        "g30": lambda: _build(30, 0.3, seed=0),
        "g50": lambda: _build(50, 0.2, seed=3),
        "g80": lambda: _build(80, 0.1, seed=1),
        "g120": lambda: _build(120, 0.08, seed=4),
        "g200": lambda: _build(200, 0.05, seed=2),
        "g400": lambda: _build(400, 0.03, seed=5),
        "g800": lambda: _build(800, 0.015, seed=6),
        "g1600": lambda: _build(1600, 0.0075, seed=7),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.vertices)} vertices / {len(problem.edges)} edges"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    from optimaru import max_cut

    solved = max_cut.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)
    # max_cut.solve returns a new model; the runner scores the one it handed in.
    for name in type(solved).model_fields:
        setattr(problem, name, getattr(solved, name))


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.max_cut import solve

    return solve(problem, time_limit_seconds=seconds, random_seed=seed, num_workers=workers)


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
