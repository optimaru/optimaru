"""Engine vs CP-SAT comparison for sports-league-scheduling.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`sports_league.constraint_costs`).
Run: `uv run python -m sports_league_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "sports-league-scheduling"
PACKAGE = "sports_league"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def instances() -> dict[str, Callable[[], Any]]:
    import sports_league

    return {"demo": lambda: sports_league.demo_schedule()}


def describe(problem: Any) -> str:
    return "Java quickstart demo league"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import sports_league

    sports_league.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from sports_league.models import LeagueScheduleModel

    from sports_league_cpsat import solve

    solved = solve(
        LeagueScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()



# --- Timefold quickstart (benchmarks/timefold-quickstarts) ---------------------

TIMEFOLD = "sports-league-scheduling"


def to_timefold(problem: Any) -> dict:
    teams, rounds = problem.teams, problem.rounds
    distance = problem.distance_to_team
    return {
        "rounds": [{"index": r.index, "weekendOrHoliday": r.weekend_or_holiday} for r in rounds],
        "teams": [
            {
                "id": t.id,
                "name": t.name,
                "distanceToTeam": {other.id: distance[i][j] for j, other in enumerate(teams) if j != i},
            }
            for i, t in enumerate(teams)
        ],
        "matches": [
            {
                "id": m.id,
                "homeTeam": teams[m.home_team].id,
                "awayTeam": teams[m.away_team].id,
                "classicMatch": m.classic_match,
                "round": rounds[m.round].index if m.round >= 0 else None,
            }
            for m in problem.matches
        ],
    }


def from_timefold(problem: Any, result: dict) -> None:
    index = {str(r.index): i for i, r in enumerate(problem.rounds)}
    chosen = result["assignment"]["Match"]
    problem.set_assignment(
        [index[chosen[m.id]["round"]] if chosen[m.id]["round"] is not None else -1 for m in problem.matches]
    )


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
