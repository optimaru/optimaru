"""Engine vs CP-SAT comparison for container loading.

Both solvers get the same instances and wall-clock budget. The instances are containers
cut into boxes (`instances.perfect_packing`), so the fewest containers is known for each.
The engine places items by decoding loading orders; the CP-SAT model (`model.py`) places
them freely, and beyond `model.MAX_ITEMS` items it is not built. Both answers are checked
as loadings and scored from their positions by `evaluator`, not by the engine's decoder.
Run: `uv run container-loading-compare --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli, seeded

from . import instances as generator
from . import model as cpsat

NAME = "container-loading"
#: Scored from the positions, for both solvers (see `evaluator`).
PACKAGE = "container_loading_cpsat.evaluator"
CPSAT_EXACT = True
# The model is exact for what it covers; what it does and does not prove is in the README.
CPSAT_CAVEAT = ""

# to_core() builds a fresh problem per call; remember which model each one came from so the
# CP-SAT side can solve the same instance.
_MODELS: dict[int, Any] = {}


def _build(containers: int, per_container: int, seed: int) -> Any:
    model = generator.perfect_packing(containers, per_container, seed)
    problem = model.to_core()
    _MODELS[id(problem)] = model
    return problem


def instances() -> dict[str, Callable[[], Any]]:
    # The first seed of a scale is the instance of that name; the others are `<scale>-2`, `<scale>-3`, ...
    return seeded(
        {
            "tiny": (lambda seed: _build(1, 5, seed=seed), (1, 2, 3, 4)),
            "small": (lambda seed: _build(2, 4, seed=seed), (1, 2, 3, 4)),
            "medium": (lambda seed: _build(3, 5, seed=seed), (1, 2, 3, 4)),
            "large": (lambda seed: _build(4, 8, seed=seed), (1, 2, 3, 4)),
            "xlarge": (lambda seed: _build(20, 25, seed=seed), (1, 2, 3, 4)),
        }
    )


def describe(problem: Any) -> str:
    fewest = generator.optimum(_MODELS[id(problem)])
    return f"{len(problem.items)} items / {len(problem.containers)} containers (optimum {fewest})"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    from optimaru import container_loading

    container_loading.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    return cpsat.solve(_MODELS[id(problem)], seconds=seconds, seed=seed, workers=workers).solved


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
