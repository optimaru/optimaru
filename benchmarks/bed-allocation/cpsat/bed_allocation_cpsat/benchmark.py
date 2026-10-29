"""Use-case-local engine/CP-SAT comparison.

The formulation, deterministic comparison instances, and CLI entry point live
together in this use-case package.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from datetime import date
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "bed-allocation"
PACKAGE = "bed_allocation"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""

SPECIALTIES = ["Specialty1", "Specialty2", "Specialty3"]
EQUIPMENT_TYPES = ["telemetry", "television", "oxygen", "nitrogen"]
AGE_BUCKETS = [
    (0, 10),
    (11, 20),
    (21, 30),
    (31, 40),
    (41, 50),
    (51, 60),
    (61, 70),
    (71, 80),
    (81, 90),
    (91, 100),
    (101, 109),
]
AGE_CUMULATIVE = [0.10, 0.19, 0.26, 0.36, 0.45, 0.53, 0.61, 0.74, 0.82, 0.91, 1.0]
FIRST_DAY = date(2024, 1, 1).toordinal()


def _draw(rng: random.Random, cumulative: list[float]) -> int:
    value = rng.random()
    for index, threshold in enumerate(cumulative):
        if value <= threshold:
            return index
    return len(cumulative) - 1


def _distinct_equipment(
    rng: random.Random, cumulative: list[float], count: int
) -> list[str]:
    chosen: list[str] = []
    while len(chosen) < count:
        item = EQUIPMENT_TYPES[_draw(rng, cumulative)]
        if item not in chosen:
            chosen.append(item)
    return chosen


def _build(room_count: int, last_day: int, seed: int) -> Any:
    """Structural transcription of the C++ demo's ``generateDemoData``.
    Python's RNG stream differs from mt19937_64, so this matches the Java demo
    in scale and distribution rather than bit-for-bit."""
    import bed_allocation as ba

    rng = random.Random(seed)
    department = ba.Department(
        "1", "Department", {"Specialty1": 1, "Specialty2": 2, "Specialty3": 2}
    )

    capacities = [1] * room_count
    order = list(range(room_count))
    rng.shuffle(order)
    for i, room in enumerate(order):
        capacities[room] = (
            1 if i < 0.8 * room_count else (2 if i < 0.9 * room_count else 3)
        )

    equipment: list[list[str]] = [[] for _ in range(room_count)]
    rng.shuffle(order)
    for i in range(int(0.76 * room_count)):
        count = _draw(rng, [0.11, 0.27, 0.69, 1.0]) + 1
        pool = list(EQUIPMENT_TYPES)
        rng.shuffle(pool)
        equipment[order[i]] = pool[:count]

    rooms = [
        ba.Room(
            str(r),
            f"Room{r}",
            0,
            capacities[r],
            ba.GenderLimitation.SAME_GENDER,
            equipment[r],
        )
        for r in range(room_count)
    ]
    beds = [
        ba.Bed(f"{r}-bed{i}", r, i)
        for r in range(room_count)
        for i in range(capacities[r])
    ]

    stays = []
    last_ordinal = FIRST_DAY + last_day
    for _ in range(room_count):
        current = FIRST_DAY
        while current < last_ordinal:
            nights = _draw(rng, [0.05, 0.30, 0.95, 1.0]) + 1
            departure = min(current + nights, last_ordinal)
            index = len(stays)
            gender = ba.Gender.MALE if rng.randrange(2) == 0 else ba.Gender.FEMALE
            low, high = AGE_BUCKETS[_draw(rng, AGE_CUMULATIVE)]
            required_count = _draw(rng, [0.12, 0.59, 1.0])
            required: list[str] = []
            if required_count == 1:
                required = _distinct_equipment(rng, [0.22, 0.47, 0.72, 1.0], 1)
            elif required_count == 2:
                required = _distinct_equipment(rng, [0.13, 0.29, 0.49, 1.0], 2)
            preferred = _distinct_equipment(
                rng, [0.32, 0.62, 0.90, 1.0], _draw(rng, [0.29, 0.82, 0.98, 1.0]) + 1
            )
            stays.append(
                ba.Stay(
                    f"stay-{index}",
                    f"patient-{index}",
                    gender,
                    low + rng.randrange(high - low + 1),
                    current,
                    departure,
                    SPECIALTIES[_draw(rng, [0.27, 0.63, 1.0])],
                    _draw(rng, [0.34, 0.68, 1.0]) + 1,
                    required,
                    preferred,
                )
            )
            current = departure + 1

    return ba.BedPlan([department], rooms, beds, stays)


def instances() -> dict[str, Callable[[], Any]]:
    return {
        "demo": lambda: _build(10, 27, seed=0),
        "medium": lambda: _build(24, 27, seed=1),
        "large": lambda: _build(48, 41, seed=2),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.stays)} stays / {problem.bed_count()} beds / {len(problem.rooms)} rooms"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import bed_allocation as ba

    config = ba.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    ba.Solver(config).solve(problem)


def self_report(problem: Any) -> tuple[float, ...]:
    """Folded to the two levels `bed_allocation.constraints.score` reports
    (medium rides in soft at 1e9 — see the package docstring)."""
    score = problem.score
    return (score.hard, score.medium * 10**9 + score.soft)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from bed_allocation.models import BedPlanModel

    from bed_allocation_cpsat import solve

    solved = solve(
        BedPlanModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    core = solved.to_core()
    # ``BedPlanModel`` represents both the uninitialized value and the solved
    # "no bed" sentinel as ``None``. A CP-SAT result is fully initialized, so
    # restore the sentinel before the shared scorer sees it.
    assignment = core.assignment()
    no_bed = core.no_bed_value()
    for index, stay in enumerate(solved.stays):
        if stay.bed_id is None:
            assignment[index] = no_bed
    core.set_assignment(assignment)
    return core


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
