"""Use-case-local engine/CP-SAT comparison.

The deterministic comparison instances and the CLI entry point live in this use-case
package; the CP-SAT formulation is `box_sizing_cpsat.model`.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli, seeded

NAME = "box-sizing"
PACKAGE = "optimaru.box_sizing"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""

# The largest candidate: it takes every parcel, so every instance is feasible.
LARGEST = (600, 450, 400)


def _build(box_count: int, parcel_count: int, max_box_types: int, seed: int) -> Any:
    """A warehouse of the demo's kind at another scale: `box_count` candidate sizes drawn
    between a small mailer and `LARGEST` (which is always one of them), and the demo's
    parcels (`optimaru.box_sizing.demo`) for the same seed."""
    import optimaru.box_sizing as bs
    from optimaru.box_sizing.models import BoxModel

    rng = random.Random(1_000_003 * seed + box_count)
    sizes = {LARGEST}
    while len(sizes) < box_count:
        length = 10 * rng.randint(15, 60)
        width = 10 * rng.randint(10, min(45, length // 10))
        height = 10 * rng.randint(4, min(40, width // 10))
        sizes.add((length, width, height))
    ordered = sorted(sizes, key=lambda size: (size[0] * size[1] * size[2], size))
    model = bs.generate_demo_data(seed=seed, parcel_count=parcel_count, max_box_types=max_box_types)
    data = model.model_dump()
    data["boxes"] = [
        BoxModel(
            id=f"BOX-{index + 1:03d}",
            length=length,
            width=width,
            height=height,
            max_weight=20_000 if length * width * height < 20_000_000 else 30_000,
        ).model_dump()
        for index, (length, width, height) in enumerate(ordered)
    ]
    return type(model).model_validate(data).to_core()


def _demo() -> Any:
    import optimaru.box_sizing as bs

    return bs.generate_demo_data().to_core()


def instances() -> dict[str, Callable[[], Any]]:
    # The first seed of a scale is the instance of that name; the others are `<scale>-2`, `<scale>-3`, ...
    return seeded(
        {
            "small": (lambda seed: _build(12, 40, 3, seed=seed), (1, 2, 3)),
            # The use case's demo instance: 24 candidates, 120 parcel sizes, 6 box types.
            "demo": _demo,
            "medium": (lambda seed: _build(40, 300, 6, seed=seed), (4, 5, 6, 7)),
            "large": (lambda seed: _build(80, 1000, 8, seed=seed), (8, 9, 10, 11)),
            "xlarge": (lambda seed: _build(150, 3000, 10, seed=seed), (12, 13, 14, 15)),
            "huge": (lambda seed: _build(300, 8000, 12, seed=seed), (16, 17, 18, 19)),
        }
    )


def describe(problem: Any) -> str:
    return f"{problem.parcel_count()} parcels / {problem.box_count()} boxes / {problem.max_box_types} types"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import optimaru.box_sizing as bs

    bs.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from optimaru.box_sizing.models import BoxSizingProblemModel

    from .model import solve

    solved = solve(
        BoxSizingProblemModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        parallel_solver_count=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
