"""Engine vs CP-SAT comparison for quadratic assignment.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`quadratic_assignment.constraint_costs`).
The CP-SAT reference model is O(n^4), so the instances stay small (QAPLIB-size
runs live in `bench/run_bench.py`).
Run: `uv run python -m quadratic_assignment_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "quadratic-assignment"
PACKAGE = "optimaru.quadratic_assignment"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""

# The web demo's plant layout (web/src/lib/demo-data/quadratic-assignment.json).
PLANT = {
    "facilities": ["Assembly", "Warehouse", "Paint", "Inspection", "Packing", "Office"],
    "locations": ["North", "East", "South", "West", "Center", "Annex"],
    "fixed_assignments": {"Annex": "Office"},
    "forbidden_assignments": [{"facility": "Assembly", "location": "Annex"}],
    "facility_demands": {"Assembly": 8, "Warehouse": 7, "Paint": 6, "Inspection": 4, "Packing": 5, "Office": 2},
    "location_capacities": {"North": 8, "East": 8, "South": 8, "West": 8, "Center": 8, "Annex": 2},
    "flows": [
        [0, 8, 5, 2, 7, 1],
        [8, 0, 3, 4, 9, 2],
        [5, 3, 0, 8, 4, 1],
        [2, 4, 8, 0, 6, 3],
        [7, 9, 4, 6, 0, 2],
        [1, 2, 1, 3, 2, 0],
    ],
    "distances": [
        [0, 2, 5, 4, 2, 6],
        [2, 0, 3, 5, 1, 4],
        [5, 3, 0, 2, 3, 5],
        [4, 5, 2, 0, 4, 3],
        [2, 1, 3, 4, 0, 3],
        [6, 4, 5, 3, 3, 0],
    ],
}


def self_report(problem: Any) -> tuple[float, ...]:
    # The engine's own score of its answer (the model's score fields after
    # quadratic_assignment.solve), cross-checked against the rescored one.
    return (problem.score_hard, problem.score_soft)


def _plant() -> Any:
    from optimaru.quadratic_assignment.models import QuadraticAssignmentProblemModel

    return QuadraticAssignmentProblemModel.model_validate(PLANT)


def _random(size: int, seed: int) -> Any:
    from optimaru.quadratic_assignment.models import QuadraticAssignmentProblemModel

    rng = random.Random(seed)
    facilities = [f"F{i}" for i in range(size)]
    locations = [f"L{i}" for i in range(size)]
    flows = [[0] * size for _ in range(size)]
    distances = [[0] * size for _ in range(size)]
    for a in range(size):
        for b in range(a + 1, size):
            flows[a][b] = flows[b][a] = rng.randint(0, 9)
            distances[a][b] = distances[b][a] = rng.randint(1, 9)
    return QuadraticAssignmentProblemModel(
        facilities=facilities,
        locations=locations,
        flows=flows,
        distances=distances,
        fixed_assignments={locations[0]: facilities[size - 1]},
        forbidden_assignments=[{"facility": facilities[0], "location": locations[1]}],
    )


def instances() -> dict[str, Callable[[], Any]]:
    return {
        "plant6": _plant,
        "random8": lambda: _random(8, seed=0),
        "random10": lambda: _random(10, seed=1),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.facilities)} facilities"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    from optimaru import quadratic_assignment

    quadratic_assignment.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.quadratic_assignment import solve

    return solve(problem, time_limit_seconds=seconds, random_seed=seed, num_workers=workers)


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
