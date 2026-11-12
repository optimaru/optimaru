"""Dump the engine/CP-SAT comparison instances to a plain-text format.

The instances the ``conference-scheduling-compare`` harness builds in Python
(``conference_scheduling_cpsat.benchmark.instances``: the demo conference and the seeded
generator's rungs) are written to ``<outdir>/<name>.cstxt`` so the C++ engine-only bench
(``benchmarks/conference-scheduling/cpp/cs_quality_bench.cpp``) solves EXACTLY the same
problems; the bench has no generator of its own.

Run from the repo root:

    uv run --project benchmarks/conference-scheduling/cpsat \
        python benchmarks/conference-scheduling/dump_instances.py <outdir> [--check <cs_quality_bench>]

Each line it prints ends with the score of the fixed assignment ``talk i -> slotRoom
(7 * i) % slotRoomCount`` under the use case's own scorer; ``cs_quality_bench crosscheck <file>``
prints the C++ score of the same assignment, which must be equal. ``--check`` with the path of
the built bench runs it on every file and fails on a difference in the total or in any
constraint's raw cost (CI does, so the writer here
and the reader in the bench cannot drift apart).

Format (whitespace-separated tokens): a string is double-quoted, with ``\\`` before a
quote or a backslash inside it (C++ ``std::quoted``); a list is its length and then its
items; ``ints`` is a list of integers and ``strs`` a list of strings.

    CS1
    name <string>
    minimumConsecutiveTalksPauseInMinutes <n>
    talkTypes <strs>
    timeslots <n>
      <id> <startMinute> <endMinute> <ints talkTypes> <strs tags>
    rooms <n>
      <id> <name> <capacity> <ints talkTypes> <ints unavailableTimeslots> <strs tags>
    speakers <n>
      <id> <name> <ints unavailableTimeslots> <eight strs>
    talks <n>
      <code> <title> <talkType> <ints speakers> <strs themeTrackTags> <strs sectorTags>
      <strs audienceTypes> <audienceLevel> <strs contentTags> <language> <eight strs>
      <strs mutuallyExclusiveTalksTags> <ints prerequisiteTalks> <favoriteCount>
      <crowdControlRisk> <slotRoom|-1> <pinned 0|1>

The ``eight strs`` are the required, preferred, prohibited and undesired timeslot tags and
then the required, preferred, prohibited and undesired room tags.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from conference_scheduling_cpsat import benchmark
from optimaru import conference_scheduling
from optimaru.conference_scheduling.models import ConferenceScheduleModel

SECTIONS = ("timeslots", "rooms", "speakers", "talks")
# The list-valued fields of a talk, each written as its length and then its items.
TALK_LISTS = (
    "speakers",
    "theme_track_tags",
    "sector_tags",
    "audience_types",
    "content_tags",
    "required_timeslot_tags",
    "preferred_timeslot_tags",
    "prohibited_timeslot_tags",
    "undesired_timeslot_tags",
    "required_room_tags",
    "preferred_room_tags",
    "prohibited_room_tags",
    "undesired_room_tags",
    "mutually_exclusive_talks_tags",
    "prerequisite_talks",
)
PREFERENCE_TAGS = (
    "required_timeslot_tags",
    "preferred_timeslot_tags",
    "prohibited_timeslot_tags",
    "undesired_timeslot_tags",
    "required_room_tags",
    "preferred_room_tags",
    "prohibited_room_tags",
    "undesired_room_tags",
)


def _string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _ints(values) -> str:
    return " ".join([str(len(values)), *(str(value) for value in values)])


def _strs(values) -> str:
    return " ".join([str(len(values)), *(_string(value) for value in values)])


def to_text(model: ConferenceScheduleModel) -> str:
    lines = [
        "CS1",
        f"name {_string(model.name)}",
        f"minimumConsecutiveTalksPauseInMinutes {model.minimum_consecutive_talks_pause_in_minutes}",
        f"talkTypes {_strs(model.talk_types)}",
        f"timeslots {len(model.timeslots)}",
    ]
    for slot in model.timeslots:
        parts = [_string(slot.id), str(slot.start_minute), str(slot.end_minute)]
        lines.append(" ".join([*parts, _ints(slot.talk_types), _strs(slot.tags)]))
    lines.append(f"rooms {len(model.rooms)}")
    for room in model.rooms:
        parts = [_string(room.id), _string(room.name), str(room.capacity)]
        lines.append(" ".join([*parts, _ints(room.talk_types), _ints(room.unavailable_timeslots), _strs(room.tags)]))
    lines.append(f"speakers {len(model.speakers)}")
    for speaker in model.speakers:
        parts = [_string(speaker.id), _string(speaker.name), _ints(speaker.unavailable_timeslots)]
        lines.append(" ".join(parts + [_strs(getattr(speaker, name)) for name in PREFERENCE_TAGS]))
    lines.append(f"talks {len(model.talks)}")
    for talk in model.talks:
        parts = [_string(talk.code), _string(talk.title), str(talk.talk_type), _ints(talk.speakers)]
        parts += [_strs(talk.theme_track_tags), _strs(talk.sector_tags), _strs(talk.audience_types)]
        parts += [str(talk.audience_level), _strs(talk.content_tags), _string(talk.language)]
        parts += [_strs(getattr(talk, name)) for name in PREFERENCE_TAGS]
        parts += [_strs(talk.mutually_exclusive_talks_tags), _ints(talk.prerequisite_talks)]
        parts += [str(talk.favorite_count), str(talk.crowd_control_risk), str(talk.slot_room), str(int(talk.pinned))]
        lines.append(" ".join(parts))
    return "\n".join(lines) + "\n"


def crosscheck(problem) -> tuple[str, dict[str, float]]:
    """The score of the fixed assignment the C++ bench scores in its `crosscheck` mode, and
    the non-zero raw cost of each constraint under its C++ name (the size of a reward).
    Scores a copy; a pinned entity keeps its place."""
    schedule = ConferenceScheduleModel.from_core(problem).to_core()
    cells = schedule.slot_room_count()
    current = schedule.assignment()
    pinned = [talk.pinned for talk in schedule.talks]
    schedule.set_assignment([current[i] if pinned[i] else (7 * i) % cells for i in range(len(current))])
    score = conference_scheduling.calculate_score(schedule)
    costs = {
        re.sub(r"_(\w)", lambda found: found.group(1).upper(), name): abs(sum(entry["costs"]))
        for name, entry in conference_scheduling.constraint_costs(schedule).items()
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
        path = outdir / f"{name}.cstxt"
        path.write_text(to_text(ConferenceScheduleModel.from_core(problem)), encoding="utf-8")
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
