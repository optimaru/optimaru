"""Engine vs CP-SAT comparison for meeting-scheduling.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`meeting_scheduling.constraint_costs`).
Run: `uv run python -m meeting_scheduling_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "meeting-scheduling"
PACKAGE = "meeting_scheduling"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def instances() -> dict[str, Callable[[], Any]]:
    import meeting_scheduling.models

    return {"demo": lambda: meeting_scheduling.models.demo_data().to_core()}


def describe(problem: Any) -> str:
    return "Java quickstart demo (24 meetings)"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import meeting_scheduling.models

    meeting_scheduling.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from meeting_scheduling.models import MeetingScheduleModel

    from meeting_scheduling_cpsat import solve

    solved = solve(
        MeetingScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()



# --- Timefold quickstart (benchmarks/timefold-quickstarts) ---------------------

TIMEFOLD = "meeting-scheduling"


def to_timefold(problem: Any) -> dict:
    people, grains, rooms, meetings = problem.people, problem.time_grains, problem.rooms, problem.meetings

    def attendances(meeting: Any, kind: str, members: list[int]) -> list[dict]:
        return [
            {"id": f"{meeting.id}-{kind}-{people[p].id}", "person": people[p].id, "meeting": meeting.id}
            for p in members
        ]

    assignments = []
    for a in problem.meeting_assignments:
        assigned = a.grain_room >= 0
        assignments.append(
            {
                "id": a.id,
                "meeting": meetings[a.meeting].id,
                "pinned": a.pinned,
                "startingTimeGrain": grains[problem.time_grain_pos_of(a.grain_room)].id if assigned else None,
                "room": rooms[problem.room_index_of(a.grain_room)].id if assigned else None,
            }
        )
    return {
        "people": [{"id": p.id, "fullName": p.full_name} for p in people],
        "timeGrains": [
            {
                "id": g.id,
                "grainIndex": g.grain_index,
                "dayOfYear": g.day_of_year,
                "startingMinuteOfDay": g.starting_minute_of_day,
            }
            for g in grains
        ],
        "rooms": [{"id": r.id, "name": r.name, "capacity": r.capacity} for r in rooms],
        "meetings": [
            {
                "id": m.id,
                "topic": m.topic,
                "durationInGrains": m.duration_in_grains,
                "entireGroupMeeting": m.entire_group_meeting,
                "speakers": [],
                "requiredAttendances": attendances(m, "required", m.required_attendees),
                "preferredAttendances": attendances(m, "preferred", m.preferred_attendees),
            }
            for m in meetings
        ],
        "meetingAssignments": assignments,
    }


def from_timefold(problem: Any, result: dict) -> None:
    grain = {g.id: i for i, g in enumerate(problem.time_grains)}
    room = {r.id: i for i, r in enumerate(problem.rooms)}
    chosen = result["assignment"]["MeetingAssignment"]
    assignment = []
    for a in problem.meeting_assignments:
        variables = chosen[a.id]
        if variables["startingTimeGrain"] is None or variables["room"] is None:
            assignment.append(-1)
        else:
            assignment.append(problem.grain_room_of(grain[variables["startingTimeGrain"]], room[variables["room"]]))
    problem.set_assignment(assignment)


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
