"""Use-case-local engine/CP-SAT comparison.

The formulation, deterministic comparison instances, and CLI entry point live
together in this use-case package.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import JavaRandom, run_cli

NAME = "tournament-scheduling"
PACKAGE = "tournament_scheduling"
CPSAT_EXACT = False
CPSAT_CAVEAT = (
    "unfairness is sqrt(ssd) rounded to 6 significant figures; the CP-SAT backend "
    "minimises integral ssd, which orders solutions the same way except across a rounding tie"
)

TEAM_NAMES = ["Maarten", "Geoffrey", "Lukas", "Chris", "Fred", "Radek", "Maciej"]


def _build(
    day_count: int, per_day: int, penalty_count: int, team_names: list[str]
) -> Any:
    """The Java DemoDataGenerator, via the same `Random(0)` stream the C++ demo
    replicates — so the `demo` instance is the one `ts_demo` and the C++ tests
    solve, not a lookalike."""
    import tournament_scheduling as ts

    random = JavaRandom(0)
    teams = [ts.Team(name) for name in team_names]
    days = [ts.Day(d) for d in range(day_count)]

    penalties: list[tuple[int, int]] = []
    while len(penalties) < penalty_count:
        team = random.next_int(len(teams))
        day = random.next_int(len(days))
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
        "demo": lambda: _build(18, 4, 12, TEAM_NAMES),
        "large": lambda: _build(36, 5, 30, TEAM_NAMES + ["Anna", "Bram", "Cara"]),
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
    import tournament_scheduling as ts

    config = ts.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    ts.Solver(config).solve(problem)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from tournament_scheduling.models import TournamentScheduleModel

    from tournament_scheduling_cpsat import solve

    solved = solve(
        TournamentScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()



# --- Timefold quickstart (benchmarks/timefold-quickstarts) ---------------------

TIMEFOLD = "tournament-scheduling"


def to_timefold(problem: Any) -> dict:
    # Teams are identified by their index here (the quickstart uses numeric ids).
    days = problem.days
    return {
        "teams": [{"id": i, "name": team.name} for i, team in enumerate(problem.teams)],
        "days": [{"dateIndex": day.date_index} for day in days],
        "unavailabilityPenalties": [
            {"team": p.team, "day": days[p.day].date_index} for p in problem.unavailability_penalties
        ],
        "teamAssignments": [
            {
                "id": a.id,
                "day": days[a.day].date_index,
                "indexInDate": a.index_in_day,
                "pinned": a.pinned,
                "team": a.team if a.team >= 0 else None,
            }
            for a in problem.team_assignments
        ],
    }


def from_timefold(problem: Any, result: dict) -> None:
    chosen = result["assignment"]["TeamAssignment"]
    problem.set_assignment(
        [int(chosen[str(a.id)]["team"]) if chosen[str(a.id)]["team"] is not None else -1 for a in problem.team_assignments]
    )


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
