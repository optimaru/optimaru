"""Engine vs CP-SAT comparison for sports-league-scheduling.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`optimaru.sports_league_scheduling.constraint_costs`).
Run: `uv run python -m sports_league_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import math
import random
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


def _generate(team_count: int, seed: int) -> Any:
    """A seeded league in the shape of the demo: a double round robin (every team hosts every
    other once), a few spare rounds, about 5% classic matches (a match and its return leg
    together), and home grounds spread over a 3000 km square."""
    from optimaru.sports_league_scheduling import LeagueSchedule, Match, Round, Team

    rng = random.Random(seed)
    grounds = [(rng.uniform(0, 3000), rng.uniform(0, 3000)) for _ in range(team_count)]
    distances = [[round(math.dist(a, b)) for b in grounds] for a in grounds]
    # The demo has 32 rounds for the 26 its 14 teams need. Round 0 is a Wednesday.
    round_count = 2 * (team_count - 1) + max(2, team_count // 2 - 1)
    rounds = [Round(index, weekend_or_holiday=(2 + index) % 7 >= 5) for index in range(round_count)]
    teams = [Team(str(index + 1), f"Team {index + 1}") for index in range(team_count)]
    pairs = [(home, away) for home in range(team_count) for away in range(home + 1, team_count)]
    classics = set(rng.sample(pairs, max(1, round(0.05 * len(pairs)))))
    matches = []
    for home in range(team_count):
        for away in range(team_count):
            if home == away:
                continue
            match = Match(f"{home + 1}-{away + 1}", home_team=home, away_team=away)
            match.classic_match = (min(home, away), max(home, away)) in classics
            matches.append(match)
    return LeagueSchedule(teams, rounds, matches, distances)


def instances() -> dict[str, Callable[[], Any]]:
    from optimaru import sports_league_scheduling

    return {
        "teams6": lambda: _generate(6, seed=1),
        "teams8": lambda: _generate(8, seed=2),
        "teams10": lambda: _generate(10, seed=3),
        # The demo league: 14 teams, 32 rounds, 182 matches.
        "demo": lambda: sports_league_scheduling.demo_schedule(),
        "teams18": lambda: _generate(18, seed=4),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.teams)} teams / {len(problem.rounds)} rounds / {len(problem.matches)} matches"


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
