"""A small made-up roster for trying the solver.

Every name and number here is invented for this file: numbered workers, lettered
locations and skills, three eight-hour shift codes and round demand. Seeded, so
the same arguments always give the same request.
"""

import random
from collections import Counter

from ._core import minutes
from .models import (
    MaxConsecutiveDaysRule,
    MaxWeeklyMinutesRule,
    ScheduleRequest,
    SchedulingInput,
    ShiftCoverageRule,
    ShiftModel,
    SolverSettings,
    UnassignedShiftRule,
    WorkerModel,
    default_rules,
)

__all__ = ["generate_demo_data"]

SKILLS = ("Skill A", "Skill B", "Skill C")
START_HOURS = {"Night": 0, "Morning": 8, "Evening": 16}
SHIFT_MINUTES = 8 * 60

# (shift code, location, required skill), cycled until a day's demand is met.
DAILY_DEMAND = (
    ("Morning", "Location A", "Skill A"),
    ("Evening", "Location A", "Skill A"),
    ("Morning", "Location B", "Skill B"),
    ("Evening", "Location B", "Skill B"),
    ("Night", "Location A", "Skill C"),
)


def _worker(index: int, days: int, rng: random.Random, unavailable_on: Counter, max_unavailable: int) -> WorkerModel:
    # Everyone holds two of the three skills, so each skill has spare holders
    # whatever the seed; a few workers also pick up the third.
    skills = [skill for position, skill in enumerate(SKILLS) if position != (index + 1) % len(SKILLS)]
    if rng.random() < 0.25:
        skills = list(SKILLS)

    shuffled = rng.sample(range(days), days)
    unavailable = [day for day in shuffled[:2] if unavailable_on[day] < max_unavailable]
    unavailable_on.update(unavailable)
    return WorkerModel(
        id=f"Worker {index + 1:02d}",
        skills=skills,
        unavailable_dates=sorted(unavailable),
        undesired_dates=sorted(shuffled[2:4]),
        desired_dates=sorted(shuffled[4:6]),
    )


def _shifts(day: int, count: int) -> list[ShiftModel]:
    shifts = []
    seen: Counter = Counter()
    for position in range(count):
        code, location, skill = DAILY_DEMAND[position % len(DAILY_DEMAND)]
        seen[code, location] += 1
        start = minutes(day, START_HOURS[code])
        shifts.append(
            ShiftModel(
                id=f"Day {day + 1:02d} {code} {location} {seen[code, location]}",
                start=start,
                end=start + SHIFT_MINUTES,
                location=location,
                required_skill=skill,
            )
        )
    return shifts


def generate_demo_data(seed: int = 7, workers: int = 12, days: int = 14) -> ScheduleRequest:
    """A `ScheduleRequest` for `workers` people over `days` days, nothing assigned.

    Half as many shifts as workers per day, with skills, unavailable days and
    desired and undesired days drawn from `seed`. The rules are the defaults
    plus full coverage, caps on consecutive days and weekly hours, and a soft
    wish for a Skill A holder at every location each day. Solves to hard 0 at
    the default size.
    """
    if workers < 1 or days < 1:
        raise ValueError(f"workers and days must be positive, got workers={workers}, days={days}")
    rng = random.Random(seed)
    unavailable_on: Counter = Counter()
    max_unavailable = max(1, workers // 4)
    return ScheduleRequest(
        name="Demo roster",
        timelimit=5.0,
        settings=SolverSettings(random_seed=seed),
        model_input=SchedulingInput(
            workers=[_worker(index, days, rng, unavailable_on, max_unavailable) for index in range(workers)],
            shifts=[shift for day in range(days) for shift in _shifts(day, max(1, workers // 2))],
            rules=[
                *default_rules(),
                UnassignedShiftRule(),
                MaxConsecutiveDaysRule(value=6),
                MaxWeeklyMinutesRule(minutes=40 * 60),
                ShiftCoverageRule(worker_skill="Skill A", min_count=1, weight=100.0),
            ],
        ),
    )
