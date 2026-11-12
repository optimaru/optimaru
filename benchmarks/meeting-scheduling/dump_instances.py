"""Dump the engine/CP-SAT comparison instances to a plain-text format.

The instances the ``meeting-scheduling-compare`` harness builds in Python
(``meeting_scheduling_cpsat.benchmark.instances``: the demo data and the seeded generator's
rungs) are written to ``<outdir>/<name>.mstxt`` so the C++ engine-only bench
(``benchmarks/meeting-scheduling/cpp/ms_quality_bench.cpp``) solves EXACTLY the same
problems; the bench has no generator of its own.

Run from the repo root:

    uv run --project benchmarks/meeting-scheduling/cpsat \
        python benchmarks/meeting-scheduling/dump_instances.py <outdir> [--check <ms_quality_bench>]

Each line it prints ends with the score of the fixed assignment ``assignment i -> grainRoom
(7 * i) % grainRoomCount`` under the use case's own scorer; ``ms_quality_bench crosscheck <file>``
prints the C++ score of the same assignment, which must be equal. ``--check`` with the path of
the built bench runs it on every file and fails on a difference in the total or in any
constraint's raw cost (CI does, so the writer here
and the reader in the bench cannot drift apart).

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

import argparse
import re
import subprocess
import sys
from pathlib import Path

from meeting_scheduling_cpsat import benchmark
from optimaru import meeting_scheduling
from optimaru.meeting_scheduling.models import MeetingScheduleModel

SECTIONS = ("people", "timeGrains", "rooms", "meetings", "meetingAssignments")


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


def crosscheck(problem) -> tuple[str, dict[str, float]]:
    """The score of the fixed assignment the C++ bench scores in its `crosscheck` mode, and
    the non-zero raw cost of each constraint under its C++ name (the size of a reward).
    Scores a copy; a pinned entity keeps its place."""
    schedule = MeetingScheduleModel.from_core(problem).to_core()
    cells = schedule.grain_room_count()
    current = schedule.assignment()
    pinned = [assignment.pinned for assignment in schedule.meeting_assignments]
    schedule.set_assignment([current[i] if pinned[i] else (7 * i) % cells for i in range(len(current))])
    score = meeting_scheduling.calculate_score(schedule)
    costs = {
        re.sub(r"_(\w)", lambda found: found.group(1).upper(), name): abs(sum(entry["costs"]))
        for name, entry in meeting_scheduling.constraint_costs(schedule).items()
    }
    return f"hard={score.hard} soft={score.soft:.0f}", {name: cost for name, cost in costs.items() if cost}


def bench_crosscheck(bench: str, path: Path) -> tuple[str, dict[str, float]]:
    """What the C++ bench scores the fixed assignment of the dumped file at: the total and
    its non-zero `Breakdown` members."""
    proc = subprocess.run([bench, "crosscheck", str(path)], capture_output=True, text=True)
    found = re.search(r"_BENCH_CROSSCHECK (.*)", proc.stdout)
    if proc.returncode != 0 or found is None:
        raise SystemExit(f"{bench} crosscheck {path} failed ({proc.returncode}): {proc.stderr.strip()}")
    units = re.findall(r"_BENCH breakdown \w\.(\w+) units=(-?\d+)", proc.stdout)
    return found.group(1).strip(), {name: abs(float(value)) for name, value in units}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Dump the comparison instances for the C++ bench.")
    parser.add_argument("outdir", nargs="?", default="out")
    parser.add_argument("--check", metavar="BENCH", help="the built C++ bench: its score of each file must match")
    args = parser.parse_args(argv[1:])
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    differing = []
    for name, factory in benchmark.instances().items():
        problem = factory()
        path = outdir / f"{name}.mstxt"
        path.write_text(to_text(MeetingScheduleModel.from_core(problem)), encoding="utf-8")
        expected, costs = crosscheck(problem)
        print(f"{name}: {benchmark.describe(problem)} -> {path} (crosscheck {expected})")
        if not args.check:
            continue
        got, units = bench_crosscheck(args.check, path)
        if got != expected:
            differing.append(f"{name}: Python {expected}, C++ {got}")
        for constraint in sorted(costs.keys() | units.keys()):
            if costs.get(constraint, 0) != units.get(constraint, 0):
                differing.append(
                    f"{name}: {constraint}: Python {costs.get(constraint, 0)}, C++ {units.get(constraint, 0)}"
                )
    if differing:
        raise SystemExit("the C++ bench reads another problem than was dumped:\n  " + "\n  ".join(differing))
    if args.check:
        print("cross-check: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
