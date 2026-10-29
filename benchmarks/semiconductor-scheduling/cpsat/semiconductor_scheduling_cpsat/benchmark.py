"""Engine vs CP-SAT comparison for semiconductor-scheduling.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`semiconductor_scheduling.constraint_costs`).
Run: `uv run python -m semiconductor_scheduling_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "semiconductor-scheduling"
PACKAGE = "semiconductor_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def _build(lot_count: int, seed: int) -> Any:
    from semiconductor_scheduling.models import (
        LotModel,
        MachineModel,
        MachineOptionModel,
        OperationModel,
        SemiconductorScheduleModel,
    )

    rng = random.Random(seed)
    families = ["PHOTO", "ETCH", "DIFF"]
    machines = [MachineModel(id=f"{family}{m}", group=family) for family in families for m in range(2)]
    lots = []
    for lot in range(lot_count):
        operations = []
        for step in range(4):
            family = families[step % len(families)]
            operations.append(
                OperationModel(
                    id=f"S{step}",
                    family=family,
                    eligible_machines=[
                        MachineOptionModel(machine_id=f"{family}{m}", duration=rng.randint(2, 8)) for m in range(2)
                    ],
                )
            )
        lots.append(
            LotModel(id=f"L{lot}", product=f"P{lot % 3}", release_time=rng.randint(0, 10),
                     due_time=rng.randint(40, 80), operations=operations)
        )
    return SemiconductorScheduleModel(lots=lots, machines=machines)


def instances() -> dict[str, Callable[[], Any]]:
    return {"lots6": lambda: _build(6, seed=0), "lots12": lambda: _build(12, seed=1)}


def describe(problem: Any) -> str:
    return f"{len(problem.lots)} lots / {len(problem.machines)} machines"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import semiconductor_scheduling

    semiconductor_scheduling.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from semiconductor_scheduling_cpsat import solve

    return solve(problem, time_limit_seconds=seconds, random_seed=seed, num_workers=workers)


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
