"""Engine vs CP-SAT comparison for sports-league-scheduling.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`optimaru.sports_league_scheduling.constraint_costs`).
Run: `uv run python -m sports_league_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "sports-league-scheduling"
PACKAGE = "optimaru.sports_league_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def instances() -> dict[str, Callable[[], Any]]:
    from optimaru import sports_league_scheduling

    return {"demo": lambda: sports_league_scheduling.demo_schedule()}


def describe(problem: Any) -> str:
    return "demo league (14 teams)"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    from optimaru import sports_league_scheduling

    sports_league_scheduling.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.sports_league_scheduling import solve

    from optimaru.sports_league_scheduling.models import LeagueScheduleModel

    solved = solve(
        LeagueScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
