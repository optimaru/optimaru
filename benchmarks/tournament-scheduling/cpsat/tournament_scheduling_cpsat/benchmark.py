"""Use-case-local engine/CP-SAT comparison.

The formulation, deterministic comparison instances, and CLI entry point live
together in this use-case package.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "tournament-scheduling"
PACKAGE = "optimaru.tournament_scheduling"
CPSAT_EXACT = False
CPSAT_CAVEAT = (
    "the two imbalance costs are sqrt(ssd) rounded to 6 significant figures; CP-SAT "
    "minimises the integral surrogate scale * ssd instead (not raw-identical), so both "
    "answers are rescored by the engine"
)

TEAM_NAMES = ["Maarten", "Geoffrey", "Lukas", "Chris", "Fred", "Radek", "Maciej"]


def _build(day_count: int, per_day: int, penalty_count: int, team_names: list[str]) -> Any:
    """A tournament with `penalty_count` distinct (team, day) unavailabilities
    drawn from a seeded ``random.Random`` (the same instance on every run)."""
    import optimaru.tournament_scheduling as ts

    rng = random.Random(0)
    teams = [ts.Team(name) for name in team_names]
    days = [ts.Day(d) for d in range(day_count)]

    penalties: list[tuple[int, int]] = []
    while len(penalties) < penalty_count:
        team = int(rng.random() * len(teams))
        day = int(rng.random() * len(days))
        if (team, day) not in penalties:
            penalties.append((team, day))

    assignments = []
    identifier = 0
    for day in range(day_count):
        for index in range(per_day):
            assignments.append(ts.TeamAssignment(identifier, day, index))
            identifier += 1

    return ts.TournamentSchedule(
        teams,
        days,
        [ts.UnavailabilityPenalty(team, day) for team, day in penalties],
        assignments,
    )


XLARGE_TEAM_NAMES = TEAM_NAMES + [
    "Anna",
    "Bram",
    "Cara",
    "Dana",
    "Elin",
    "Finn",
    "Gwen",
    "Hugo",
    "Iris",
    "Jara",
    "Kian",
    "Lena",
    "Milo",
]


def instances() -> dict[str, Callable[[], Any]]:
    return {
        "small": lambda: _build(12, 4, 8, TEAM_NAMES),
        "demo": lambda: _build(18, 4, 12, TEAM_NAMES),
        "medium": lambda: _build(24, 4, 20, XLARGE_TEAM_NAMES[:8]),
        "large": lambda: _build(36, 5, 30, TEAM_NAMES + ["Anna", "Bram", "Cara"]),
        "larger": lambda: _build(60, 6, 90, XLARGE_TEAM_NAMES[:14]),
        # demo and large saturate: the engine reaches their provable optima
        # (demo -1.54919, large 0.0) on every seed well inside a 10s budget,
        # so they cannot rank solver changes. xlarge (800 slots / 20 teams /
        # 100 days) still has headroom at 10s and separates seeds — it is the
        # instance quality tuning is measured on.
        "xlarge": lambda: _build(100, 8, 200, XLARGE_TEAM_NAMES),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.team_assignments)} slots / {problem.team_count()} teams / {problem.day_count()} days"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import optimaru.tournament_scheduling as ts

    config = ts.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    ts.Solver(config).solve(problem)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.tournament_scheduling import solve

    from optimaru.tournament_scheduling.models import TournamentScheduleModel

    solved = solve(
        TournamentScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
