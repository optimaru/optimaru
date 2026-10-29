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

NAME = "flight-crew-scheduling"
PACKAGE = "flight_crew"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""

PILOT = "Pilot"
ATTENDANT = "Flight attendant"
MINUTES_PER_DAY = 24 * 60

AIRPORTS = [
    ("LHR", 51.4775, -0.461389),
    ("JFK", 40.639722, -73.778889),
    ("CNF", -19.624444, -43.971944),
    ("BRU", 50.901389, 4.484444),
    ("ATL", 33.636667, -84.428056),
    ("BNE", -27.383333, 153.118333),
]
HOURS = [
    [0, 8, 12, 13, 9, 21],
    [8, 0, 10, 14, 6, 20],
    [12, 10, 0, 19, 10, 19],
    [13, 14, 19, 0, 9, 21],
    [9, 6, 10, 9, 0, 18],
    [21, 20, 19, 21, 18, 0],
]


def _build(count_days: int, count_flights: int, seed: int) -> Any:
    """Structural transcription of the C++ demo's ``generateDemoData``.

    Python's RNG stream differs from mt19937_64, so — like the C++ demo's own
    note about java.util.Random — the instance matches the Java demo in scale
    and distribution rather than bit-for-bit.
    """
    import flight_crew as fcs

    rng = random.Random(seed)
    airport_count = len(AIRPORTS)
    home0 = rng.randrange(airport_count)
    home1 = home0
    while home1 == home0:
        home1 = rng.randrange(airport_count)
    remaining = [a for a in range(airport_count) if a not in (home0, home1)]

    routes: list[tuple[int, int]] = []
    while len(routes) < count_flights:
        limit = count_flights - len(routes)
        route_size = 0
        while route_size == 0 or route_size > limit:
            route_size = rng.choice((2, 4, 6))
        home = home0 if rng.randrange(2) == 0 else home1
        current = rng.choice(remaining)
        routes.append((home, current))
        for _ in range(route_size - 2):
            nxt = rng.choice(remaining)
            while nxt == current:
                nxt = rng.choice(remaining)
            routes.append((current, nxt))
            current = nxt
        routes.append((current, home))

    flight_day = [f % count_days for f in range(count_flights)]
    rng.shuffle(flight_day)

    flights = []
    for f in range(count_flights):
        departure = flight_day[f] * MINUTES_PER_DAY + rng.randrange(23) * 60
        if departure < 1:
            departure = 1 + rng.randrange(4) * 60
        arrival = departure + 60 * HOURS[routes[f][0]][routes[f][1]]
        flights.append(
            fcs.Flight(
                f"Flight {f + 1}", routes[f][0], departure, routes[f][1], arrival
            )
        )

    assignments = []
    assignment_id = 0
    for f in range(count_flights):
        skills = [PILOT, PILOT, ATTENDANT, ATTENDANT]
        if AIRPORTS[routes[f][0]][0] == "CNF" or AIRPORTS[routes[f][1]][0] == "CNF":
            skills.append(ATTENDANT)
        for index, skill in enumerate(skills, start=1):
            assignment_id += 1
            assignments.append(
                fcs.FlightAssignment(str(assignment_id), f, index, skill)
            )

    flight_airports: list[int] = []
    for f in range(count_flights):
        if routes[f][0] not in flight_airports:
            flight_airports.append(routes[f][0])

    employees = []
    employee_id = 0
    for airport in flight_airports:
        for _ in range(2):
            skills = [PILOT, PILOT, ATTENDANT, ATTENDANT]
            if AIRPORTS[airport][0] == "CNF":
                skills.append(ATTENDANT)
            for skill in skills:
                employee_id += 1
                employees.append(
                    fcs.Employee(
                        str(employee_id), f"E{employee_id}", airport, [skill], set()
                    )
                )

    order = list(range(len(employees)))
    rng.shuffle(order)
    one_day = int(0.28 * len(employees))
    two_days = int(0.04 * len(employees))
    for k in range(min(one_day + two_days, len(employees))):
        employee = employees[order[k]]
        days = {rng.randrange(count_days)}
        while k >= one_day and len(days) < 2:
            days.add(rng.randrange(count_days))
        employees[order[k]] = fcs.Employee(
            employee.id,
            employee.name,
            employee.home_airport,
            list(employee.skills),
            days,
        )

    return fcs.FlightCrewSchedule(
        [fcs.Airport(code, code, lat, lon) for code, lat, lon in AIRPORTS],
        employees,
        flights,
        assignments,
    )


def instances() -> dict[str, Callable[[], Any]]:
    return {
        # The Java demo's own scale: 6 airports, 5 days, 14 flights.
        "demo": lambda: _build(5, 14, seed=0),
        "medium": lambda: _build(7, 28, seed=1),
        "large": lambda: _build(10, 56, seed=2),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.flight_assignments)} seats / {len(problem.employees)} crew / {len(problem.flights)} flights"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import flight_crew as fcs

    config = fcs.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    fcs.Solver(config).solve(problem)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from flight_crew.models import FlightCrewScheduleModel

    from flight_crew_cpsat import solve

    solved = solve(
        FlightCrewScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
