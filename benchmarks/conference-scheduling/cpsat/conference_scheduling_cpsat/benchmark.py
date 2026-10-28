"""Engine vs CP-SAT comparison for conference-scheduling.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`conference_scheduling.constraint_costs`).
Run: `uv run python -m conference_scheduling_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "conference-scheduling"
PACKAGE = "conference_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def instances() -> dict[str, Callable[[], Any]]:
    import conference_scheduling

    return {"demo": lambda: conference_scheduling.demo_conference()}


def describe(problem: Any) -> str:
    return "Java quickstart demo conference"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import conference_scheduling

    conference_scheduling.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from conference_scheduling.models import ConferenceScheduleModel

    from conference_scheduling_cpsat import solve

    solved = solve(
        ConferenceScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()



# --- Timefold quickstart (benchmarks/timefold-quickstarts) ---------------------

TIMEFOLD = "conference-scheduling"


def to_timefold(problem: Any) -> dict:
    import datetime

    day_zero = datetime.datetime(2026, 1, 5)  # a Monday, midnight
    timeslots, rooms, speakers, talks = problem.timeslots, problem.rooms, problem.speakers, problem.talks
    names = problem.talk_types

    def when(minute: int) -> str:
        return (day_zero + datetime.timedelta(minutes=minute)).isoformat(timespec="seconds")

    def ref(items: list, indices: list[int], key: str = "id") -> list[dict]:
        return [{key: getattr(items[i], key)} for i in indices]

    # TalkType is a record compared by name; the copy a talk carries holds
    # the compatibility sets the constraints read.
    talk_types = [
        {
            "name": name,
            "compatibleTimeslots": [{"id": t.id} for t in timeslots if k in t.talk_types],
            "compatibleRooms": [{"id": r.id} for r in rooms if k in r.talk_types],
        }
        for k, name in enumerate(names)
    ]

    def speaker(s: Any) -> dict:
        return {
            "id": s.id,
            "name": s.name,
            "unavailableTimeslots": ref(timeslots, s.unavailable_timeslots),
            "requiredTimeslotTags": list(s.required_timeslot_tags),
            "preferredTimeslotTags": list(s.preferred_timeslot_tags),
            "prohibitedTimeslotTags": list(s.prohibited_timeslot_tags),
            "undesiredTimeslotTags": list(s.undesired_timeslot_tags),
            "requiredRoomTags": list(s.required_room_tags),
            "preferredRoomTags": list(s.preferred_room_tags),
            "prohibitedRoomTags": list(s.prohibited_room_tags),
            "undesiredRoomTags": list(s.undesired_room_tags),
        }

    talk_json = []
    for talk in talks:
        assigned = talk.slot_room >= 0
        talk_json.append(
            {
                "code": talk.code,
                "title": talk.title,
                "talkType": talk_types[talk.talk_type],
                "speakers": [speaker(speakers[i]) for i in talk.speakers],
                "themeTrackTags": list(talk.theme_track_tags),
                "sectorTags": list(talk.sector_tags),
                "audienceTypes": list(talk.audience_types),
                "audienceLevel": talk.audience_level,
                "contentTags": list(talk.content_tags),
                "language": talk.language,
                "requiredTimeslotTags": list(talk.required_timeslot_tags),
                "preferredTimeslotTags": list(talk.preferred_timeslot_tags),
                "prohibitedTimeslotTags": list(talk.prohibited_timeslot_tags),
                "undesiredTimeslotTags": list(talk.undesired_timeslot_tags),
                "requiredRoomTags": list(talk.required_room_tags),
                "preferredRoomTags": list(talk.preferred_room_tags),
                "prohibitedRoomTags": list(talk.prohibited_room_tags),
                "undesiredRoomTags": list(talk.undesired_room_tags),
                "mutuallyExclusiveTalksTags": list(talk.mutually_exclusive_talks_tags),
                "prerequisiteTalks": ref(talks, talk.prerequisite_talks, "code"),
                "favoriteCount": talk.favorite_count,
                "crowdControlRisk": talk.crowd_control_risk,
                "timeslot": {"id": timeslots[problem.timeslot_index_of(talk.slot_room)].id} if assigned else None,
                "room": {"id": rooms[problem.room_index_of(talk.slot_room)].id} if assigned else None,
            }
        )
    return {
        "name": problem.name,
        "constraintProperties": {
            "minimumConsecutiveTalksPauseInMinutes": problem.minimum_consecutive_talks_pause_in_minutes
        },
        "talkTypes": talk_types,
        "timeslots": [
            {
                "id": t.id,
                "startDateTime": when(t.start_minute),
                "endDateTime": when(t.end_minute),
                "talkTypes": [{"name": names[k]} for k in t.talk_types],
                "tags": list(t.tags),
                "durationInMinutes": t.duration_in_minutes(),
            }
            for t in timeslots
        ],
        "rooms": [
            {
                "id": r.id,
                "name": r.name,
                "capacity": r.capacity,
                "talkTypes": [{"name": names[k]} for k in r.talk_types],
                "unavailableTimeslots": ref(timeslots, r.unavailable_timeslots),
                "tags": list(r.tags),
            }
            for r in rooms
        ],
        "speakers": [speaker(s) for s in speakers],
        "talks": talk_json,
    }


def from_timefold(problem: Any, result: dict) -> None:
    timeslot = {t.id: i for i, t in enumerate(problem.timeslots)}
    room = {r.id: i for i, r in enumerate(problem.rooms)}
    chosen = result["assignment"]["Talk"]
    assignment = []
    for talk in problem.talks:
        variables = chosen[talk.code]
        if variables["timeslot"] is None or variables["room"] is None:
            assignment.append(-1)
        else:
            assignment.append(problem.slot_room_of(timeslot[variables["timeslot"]], room[variables["room"]]))
    problem.set_assignment(assignment)


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
