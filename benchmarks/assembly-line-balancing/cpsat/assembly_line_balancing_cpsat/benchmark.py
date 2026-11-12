"""Use-case-local engine/CP-SAT comparison on generated SALBP-1 lines.

The formulation, deterministic comparison instances, and CLI entry point live
together in this use-case package. The public Otto et al. SALBP archive runs
through ``public_benchmark.py`` (downloaded on demand); these instances need no
download, so CI can smoke-test both solvers.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "assembly-line-balancing"
PACKAGE = "optimaru.assembly_line_balancing"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def _build(task_count: int, cycle_time: int, seed: int) -> Any:
    """A layered precedence graph (each task depends on up to three of the 12 before it),
    durations 1..30 with a few long ones, the given cycle time; no zoning."""
    import optimaru.assembly_line_balancing as alb

    rng = random.Random(seed)
    tasks = [
        alb.Task(f"T{index + 1:04d}", rng.randint(20, 60) if rng.random() < 0.2 else rng.randint(1, 30))
        for index in range(task_count)
    ]
    arcs = [
        alb.Precedence(rng.randrange(max(0, task - 12), task), task)
        for task in range(1, task_count)
        for _ in range(rng.choice([0, 1, 1, 2, 3]))
    ]
    return alb.AssemblyLineBalancingProblem(cycle_time, tasks, arcs)


def instances() -> dict[str, Callable[[], Any]]:
    return {
        "small": lambda: _build(20, 60, seed=0),
        "medium": lambda: _build(50, 100, seed=1),
        "large": lambda: _build(100, 120, seed=2),
    }


def describe(problem: Any) -> str:
    return f"{problem.task_count()} tasks / cycle {problem.cycle_time}"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import optimaru.assembly_line_balancing as alb

    alb.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.assembly_line_balancing import solve

    from optimaru.assembly_line_balancing.models import AssemblyLineBalancingProblemModel

    solved = solve(
        AssemblyLineBalancingProblemModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        parallel_solver_count=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
