"""Use-case-local engine/CP-SAT comparison.

The formulation, deterministic comparison instances, and CLI entry point live
together in this use-case package.

`es_scheduling` scores through `DecomposedCalculator` and has no
`constraint_costs` of its own, so `PACKAGE` names this package, whose
`constraint_costs` calls that calculator. It is still the engine's score.
"""

from __future__ import annotations

import sys
from collections.abc import Callable

from cpsat_plugins.benchmark import run_cli
from es_scheduling.models import ScheduleRequest

NAME = "employee-scheduling"
PACKAGE = "employee_scheduling_cpsat"
CPSAT_EXACT = False
CPSAT_CAVEAT = (
    "unfairness is sqrt(ssd) rounded to 6 significant figures; the CP-SAT objective "
    "holds it to 0.01, which orders solutions the same way except across a near-tie"
)


class Roster(ScheduleRequest):
    """A request that also keeps what the engine reported, for the crosscheck."""

    reported: tuple[int, float] | None = None


def _build(seed: int, workers: int, days: int) -> Roster:
    from es_scheduling.demo import generate_demo_data

    return Roster.model_validate(
        generate_demo_data(seed=seed, workers=workers, days=days).model_dump()
    )


def instances() -> dict[str, Callable[[], Roster]]:
    return {
        # `generate_demo_data()` with its own defaults.
        "demo": lambda: _build(seed=7, workers=12, days=14),
        "medium": lambda: _build(seed=1, workers=24, days=28),
        "large": lambda: _build(seed=2, workers=48, days=28),
    }


def describe(problem: Roster) -> str:
    shifts = problem.model_input.shifts
    days = len({shift.start // 1440 for shift in shifts})
    return f"{len(shifts)} shifts / {len(problem.model_input.workers)} workers / {days} days"


def solve_engine(problem: Roster, seconds: float, seed: int) -> None:
    from es_scheduling.models import solve_request, with_initial_solution

    problem.timelimit = seconds
    problem.settings.random_seed = seed
    solution = solve_request(problem)
    problem.model_input = with_initial_solution(problem, solution).model_input
    problem.reported = (solution.score_hard, solution.score_soft)


def self_report(problem: Roster) -> tuple[int, float] | None:
    return problem.reported


def solve_cpsat(problem: Roster, seconds: float, seed: int, workers: int) -> Roster:
    from employee_scheduling_cpsat import solve

    return solve(
        problem,
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
