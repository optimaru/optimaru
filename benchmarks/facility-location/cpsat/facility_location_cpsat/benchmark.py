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

NAME = "facility-location"
PACKAGE = "facility_location"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""

# Java DemoDataGenerator's London bounding box.
SOUTH_WEST = (51.44, -0.16)
NORTH_EAST = (51.56, -0.01)
AVERAGE_SETUP_COST = 50_000
SETUP_COST_STD_DEV = 10_000


def _build(
    facility_count: int,
    consumer_count: int,
    total_capacity: int,
    total_demand: int,
    seed: int,
) -> Any:
    """Mirrors DemoDataBuilder.build(): uniform locations in the box, equal
    capacity/demand shares, Gaussian setup costs.

    Python's RNG stream differs from both java.util.Random and the C++ demo's
    mt19937_64, so an instance matches the Java demo in scale and distribution
    rather than bit-for-bit — the same caveat the C++ demo already carries.
    """
    import facility_location as fl

    rng = random.Random(seed)
    facilities = []
    for index in range(facility_count):
        location = fl.Location(
            rng.uniform(SOUTH_WEST[0], NORTH_EAST[0]),
            rng.uniform(SOUTH_WEST[1], NORTH_EAST[1]),
        )
        setup_cost = AVERAGE_SETUP_COST + int(SETUP_COST_STD_DEV * rng.gauss(0.0, 1.0))
        facilities.append(
            fl.Facility(
                str(index + 1), location, setup_cost, total_capacity // facility_count
            )
        )

    consumers = []
    for index in range(consumer_count):
        location = fl.Location(
            rng.uniform(SOUTH_WEST[0], NORTH_EAST[0]),
            rng.uniform(SOUTH_WEST[1], NORTH_EAST[1]),
        )
        consumers.append(
            fl.Consumer(
                str(facility_count + index + 1),
                location,
                total_demand // consumer_count,
            )
        )

    return fl.FacilityLocationProblem(
        facilities,
        consumers,
        fl.Location(*SOUTH_WEST),
        fl.Location(*NORTH_EAST),
    )


def instances() -> dict[str, Callable[[], Any]]:
    return {
        # The Java quickstart's own demo scale.
        "demo": lambda: _build(30, 60, 4500, 900, seed=0),
        "medium": lambda: _build(50, 200, 15_000, 3_000, seed=1),
        "large": lambda: _build(80, 600, 45_000, 9_000, seed=2),
    }


def describe(problem: Any) -> str:
    return f"{problem.consumer_count()} cons / {problem.facility_count()} fac"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import facility_location as fl

    fl.solve(
        problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1
    )


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from facility_location.models import FacilityLocationProblemModel

    from facility_location_cpsat import solve

    solved = solve(
        FacilityLocationProblemModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()



# --- Timefold quickstart (benchmarks/timefold-quickstarts) ---------------------

TIMEFOLD = "facility-location"


def to_timefold(problem: Any) -> dict:
    def location(loc: Any) -> list[float]:
        return [loc.latitude, loc.longitude]

    facilities = problem.facilities
    return {
        "facilities": [
            {"id": f.id, "location": location(f.location), "setupCost": f.setup_cost, "capacity": f.capacity}
            for f in facilities
        ],
        "consumers": [
            {
                "id": c.id,
                "location": location(c.location),
                "demand": c.demand,
                "facility": {"id": facilities[c.facility].id} if c.facility >= 0 else None,
            }
            for c in problem.consumers
        ],
        "southWestCorner": location(problem.south_west_corner),
        "northEastCorner": location(problem.north_east_corner),
    }


def from_timefold(problem: Any, result: dict) -> None:
    index = {f.id: i for i, f in enumerate(problem.facilities)}
    chosen = result["assignment"]["Consumer"]
    problem.set_assignment(
        [index[chosen[c.id]["facility"]] if chosen[c.id]["facility"] else -1 for c in problem.consumers]
    )


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
