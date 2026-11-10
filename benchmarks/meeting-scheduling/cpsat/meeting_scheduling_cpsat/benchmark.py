"""Engine vs CP-SAT comparison for meeting-scheduling.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`meeting_scheduling.constraint_costs`).
Run: `uv run python -m meeting_scheduling_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "meeting-scheduling"
PACKAGE = "optimaru.meeting_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


GRAINS_PER_DAY = 40  # 08:00 to 18:00 in quarter hours, as in the demo data
ROOM_CAPACITIES = [30, 20, 16]
DURATIONS = [8] * 10 + [12] * 7 + [16] * 7  # the demo's mix of 2, 3 and 4 hour meetings


def _generate(meeting_count: int, people_count: int, room_count: int, day_count: int, seed: int) -> Any:
    """A seeded instance in the shape of the demo data: quarter-hour time grains over working
    days, rooms of 30, 20 and 16 seats, and meetings of 2 to 4 hours of which about half have
    required attendees and about half preferred ones."""
    if people_count < 4:
        # A meeting draws at least two required attendees out of at most half the people.
        raise ValueError(f"the generator needs at least 4 people, got {people_count}")
    from optimaru.meeting_scheduling.models import (
        MeetingAssignmentModel,
        MeetingModel,
        MeetingScheduleModel,
        PersonModel,
        RoomModel,
        TimeGrainModel,
    )

    rng = random.Random(seed)
    people = list(range(people_count))
    meetings = []
    for index in range(meeting_count):
        required = rng.sample(people, rng.randint(2, min(10, people_count // 2))) if rng.random() < 0.6 else []
        others = [person for person in people if person not in required]
        preferred = rng.sample(others, rng.randint(1, min(9, len(others)))) if rng.random() < 0.45 else []
        meetings.append(
            MeetingModel(
                id=str(index),
                topic=f"Meeting {index + 1}",
                duration_in_grains=rng.choice(DURATIONS),
                required_attendees=required,
                preferred_attendees=preferred,
            )
        )
    return MeetingScheduleModel(
        people=[PersonModel(id=str(person), full_name=f"Person {person + 1}") for person in people],
        time_grains=[
            TimeGrainModel(
                id=str(grain + 1),
                grain_index=grain + 1,
                day_of_year=200 + grain // GRAINS_PER_DAY,
                starting_minute_of_day=480 + 15 * (grain % GRAINS_PER_DAY),
            )
            for grain in range(day_count * GRAINS_PER_DAY)
        ],
        rooms=[
            RoomModel(id=f"R{room + 1}", name=f"Room {room + 1}", capacity=ROOM_CAPACITIES[room % 3])
            for room in range(room_count)
        ],
        meetings=meetings,
        meeting_assignments=[MeetingAssignmentModel(id=str(index), meeting=index) for index in range(meeting_count)],
    ).to_core()


def instances() -> dict[str, Callable[[], Any]]:
    from optimaru.meeting_scheduling import models

    return {
        "small": lambda: _generate(8, 10, 2, 2, seed=1),
        "medium": lambda: _generate(16, 16, 3, 3, seed=2),
        # The demo data: 24 meetings, 20 people, 3 rooms, 4 days.
        "demo": lambda: models.demo_data().to_core(),
        "large": lambda: _generate(48, 40, 5, 5, seed=3),
        "xlarge": lambda: _generate(96, 60, 8, 6, seed=4),
    }


def describe(problem: Any) -> str:
    from optimaru.meeting_scheduling.models import MeetingScheduleModel

    model = MeetingScheduleModel.from_core(problem)
    days = len({grain.day_of_year for grain in model.time_grains})
    return (
        f"{len(model.meetings)} meetings / {len(model.rooms)} meeting rooms / {days} days / {len(model.people)} people"
    )


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    from optimaru import meeting_scheduling

    meeting_scheduling.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.meeting_scheduling import solve

    from optimaru.meeting_scheduling.models import MeetingScheduleModel

    solved = solve(
        MeetingScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
