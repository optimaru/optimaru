"""Dump the engine/CP-SAT comparison instances to a plain-text format.

The instances the ``meeting-scheduling-compare`` harness builds in Python
(``meeting_scheduling_cpsat.benchmark.instances``: the demo data and the seeded generator's
rungs) are written to ``<outdir>/<name>.mstxt`` so the C++ engine-only bench
(``benchmarks/meeting-scheduling/cpp/ms_quality_bench.cpp``) solves EXACTLY the same
problems; the bench has no generator of its own.

Run from the repo root:

    uv run --project benchmarks/meeting-scheduling/cpsat \
        python benchmarks/meeting-scheduling/dump_instances.py <outdir>

Each line it prints ends with the score of the fixed assignment ``assignment i -> grainRoom
(7 * i) % grainRoomCount`` under the use case's own scorer; ``ms_quality_bench <file> <seed> 0``
prints the C++ score of the same assignment, which must be equal.

Format (whitespace-separated tokens): a string is double-quoted, with ``\\`` before a
quote or a backslash inside it (C++ ``std::quoted``); ``ints`` is a count and then that
many integers.

    MS1
    people <n>
      <id> <fullName>
    timeGrains <n>
      <id> <grainIndex> <dayOfYear> <startingMinuteOfDay>
    rooms <n>
      <id> <name> <capacity>
    meetings <n>
      <id> <topic> <durationInGrains> <ints requiredAttendees> <ints preferredAttendees>
      <entireGroupMeeting 0|1>
    meetingAssignments <n>
      <id> <meeting> <grainRoom|-1> <pinned 0|1>
"""

from __future__ import annotations

import sys
from pathlib import Path

from meeting_scheduling_cpsat import benchmark
from optimaru import meeting_scheduling
from optimaru.meeting_scheduling.models import MeetingScheduleModel


def _string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _ints(values) -> str:
    return " ".join([str(len(values)), *(str(value) for value in values)])


def to_text(model: MeetingScheduleModel) -> str:
    lines = ["MS1", f"people {len(model.people)}"]
    lines += [f"{_string(person.id)} {_string(person.full_name)}" for person in model.people]
    lines.append(f"timeGrains {len(model.time_grains)}")
    lines += [
        f"{_string(grain.id)} {grain.grain_index} {grain.day_of_year} {grain.starting_minute_of_day}"
        for grain in model.time_grains
    ]
    lines.append(f"rooms {len(model.rooms)}")
    lines += [f"{_string(room.id)} {_string(room.name)} {room.capacity}" for room in model.rooms]
    lines.append(f"meetings {len(model.meetings)}")
    for meeting in model.meetings:
        parts = [_string(meeting.id), _string(meeting.topic), str(meeting.duration_in_grains)]
        parts += [_ints(meeting.required_attendees), _ints(meeting.preferred_attendees)]
        lines.append(" ".join([*parts, str(int(meeting.entire_group_meeting))]))
    lines.append(f"meetingAssignments {len(model.meeting_assignments)}")
    for assignment in model.meeting_assignments:
        grain_room = -1 if assignment.grain_room is None else assignment.grain_room
        lines.append(f"{_string(assignment.id)} {assignment.meeting} {grain_room} {int(assignment.pinned)}")
    return "\n".join(lines) + "\n"


def crosscheck(problem) -> str:
    """The score of the fixed assignment the C++ bench scores with a budget of 0 seconds.
    Scores a copy; a pinned assignment keeps its place."""
    schedule = MeetingScheduleModel.from_core(problem).to_core()
    cells = schedule.grain_room_count()
    current = schedule.assignment()
    pinned = [assignment.pinned for assignment in schedule.meeting_assignments]
    schedule.set_assignment([current[i] if pinned[i] else (7 * i) % cells for i in range(len(current))])
    score = meeting_scheduling.calculate_score(schedule)
    return f"hard={score.hard} soft={score.soft:.0f}"


def main(argv: list[str]) -> int:
    outdir = Path(argv[1]) if len(argv) > 1 else Path("out")
    outdir.mkdir(parents=True, exist_ok=True)
    for name, factory in benchmark.instances().items():
        problem = factory()
        path = outdir / f"{name}.mstxt"
        path.write_text(to_text(MeetingScheduleModel.from_core(problem)), encoding="utf-8")
        print(f"{name}: {benchmark.describe(problem)} -> {path} (crosscheck {crosscheck(problem)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
