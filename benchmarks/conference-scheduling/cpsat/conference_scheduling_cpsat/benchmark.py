"""Engine vs CP-SAT comparison for conference-scheduling.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`conference_scheduling.constraint_costs`).
Run: `uv run python -m conference_scheduling_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "conference-scheduling"
PACKAGE = "optimaru.conference_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


# One conference day, as in the demo: two lab slots (talk type 0) and four breakout slots (1).
DAY_SLOTS = (
    (615, 735, 0, []),
    (615, 660, 1, []),
    (690, 735, 1, []),
    (780, 900, 0, ["After lunch"]),
    (930, 975, 1, []),
    (990, 1035, 1, []),
)
BREAKOUT_CAPACITIES = [60, 240, 630, 70, 120, 300]
THEMES = ["Cloud", "AI", "Optimization"]
SECTORS = ["Orange", "Blue", "Green"]
AUDIENCES = ["Analysts", "Managers", "Programmers"]
CONTENTS = ["Constraints", "Metaheuristics", "Kubernetes", "Optimization"]


def _generate(day_count: int, breakout_rooms: int, lab_rooms: int, seed: int) -> Any:
    """A seeded conference in the shape of the demo, over several days and more rooms: a lab
    for every lab slot and lab room, breakout talks for about 80% of the breakout slots, one
    or two speakers a talk, and a few talks with a room wish, a prerequisite, a crowd-control
    risk or a tag that keeps them apart."""
    from optimaru.conference_scheduling.models import (
        ConferenceScheduleModel,
        RoomModel,
        SpeakerModel,
        TalkModel,
        TimeslotModel,
    )

    rng = random.Random(seed)
    timeslots = [
        TimeslotModel(
            id=f"T{day * len(DAY_SLOTS) + index + 1}",
            start_minute=day * 1440 + start,
            end_minute=day * 1440 + end,
            talk_types=[talk_type],
            tags=tags,
        )
        for day in range(day_count)
        for index, (start, end, talk_type, tags) in enumerate(DAY_SLOTS)
    ]
    rooms = []
    for index in range(breakout_rooms):
        capacity = BREAKOUT_CAPACITIES[index % len(BREAKOUT_CAPACITIES)]
        tags = (["Recorded"] if index % 3 != 1 else []) + (["Large"] if capacity >= 500 else [])
        rooms.append(
            RoomModel(id=f"R{index + 1}", name=f"Room {index + 1}", capacity=capacity, talk_types=[1], tags=tags)
        )
    for index in range(lab_rooms):
        number = breakout_rooms + index + 1
        rooms.append(
            RoomModel(id=f"R{number}", name=f"Lab {index + 1}", capacity=490, talk_types=[0], tags=["Recorded"])
        )

    lab_talks = day_count * 2 * lab_rooms
    talk_count = lab_talks + round(0.8 * day_count * 4 * breakout_rooms)
    speaker_count = max(4, round(0.8 * talk_count))
    speakers = [
        SpeakerModel(
            id=str(index + 1),
            name=f"Speaker {index + 1}",
            undesired_timeslot_tags=["After lunch"] if rng.random() < 0.08 else [],
        )
        for index in range(speaker_count)
    ]
    talks = []
    for index in range(talk_count):
        content = rng.choice(CONTENTS)
        wish = rng.random()
        talks.append(
            TalkModel(
                code=f"S{index + 1:03d}",
                title=f"Talk {index + 1}",
                talk_type=0 if index < lab_talks else 1,
                speakers=rng.sample(range(speaker_count), 2 if rng.random() < 0.4 else 1),
                theme_track_tags=[rng.choice(THEMES)],
                sector_tags=[rng.choice(SECTORS)],
                audience_types=[rng.choice(AUDIENCES)],
                audience_level=rng.randint(1, 3),
                content_tags=[content],
                required_room_tags=["Recorded"] if wish < 0.05 else [],
                undesired_room_tags=["Recorded"] if 0.05 <= wish < 0.10 else [],
                mutually_exclusive_talks_tags=[content] if rng.random() < 0.05 else [],
                # Only an earlier talk, so prerequisites cannot form a cycle.
                prerequisite_talks=[rng.randrange(index)] if index and rng.random() < 0.05 else [],
                favorite_count=rng.randint(0, 1000),
                crowd_control_risk=1 if rng.random() < 0.08 else 0,
            )
        )
    return ConferenceScheduleModel(
        name=f"Conference of {day_count} days",
        talk_types=["Lab", "Breakout"],
        timeslots=timeslots,
        rooms=rooms,
        speakers=speakers,
        talks=talks,
    ).to_core()


def instances() -> dict[str, Callable[[], Any]]:
    from optimaru import conference_scheduling

    return {
        # The demo conference: one day, 15 talks.
        "demo": lambda: conference_scheduling.demo_conference(),
        "days2": lambda: _generate(2, 4, 1, seed=1),
        "days3": lambda: _generate(3, 6, 2, seed=2),
        "days4": lambda: _generate(4, 8, 2, seed=3),
        "days5": lambda: _generate(5, 10, 3, seed=4),
    }


def describe(problem: Any) -> str:
    from optimaru.conference_scheduling.models import ConferenceScheduleModel

    model = ConferenceScheduleModel.from_core(problem)
    return (
        f"{len(model.talks)} talks / {len(model.rooms)} halls / "
        f"{len(model.timeslots)} timeslots / {len(model.speakers)} speakers"
    )


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    from optimaru import conference_scheduling

    conference_scheduling.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.conference_scheduling import solve

    from optimaru.conference_scheduling.models import ConferenceScheduleModel

    solved = solve(
        ConferenceScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
