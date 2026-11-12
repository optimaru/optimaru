"""Dump the engine/CP-SAT comparison instances to a plain-text format.

The instances the ``conference-scheduling-compare`` harness builds in Python
(``conference_scheduling_cpsat.benchmark.instances``: the demo conference and the seeded
generator's rungs) are written to ``<outdir>/<name>.cstxt`` so the C++ engine-only bench
(``benchmarks/conference-scheduling/cpp/cs_quality_bench.cpp``) solves EXACTLY the same
problems; the bench has no generator of its own.

Run from the repo root:

    uv run --project benchmarks/conference-scheduling/cpsat \
        python benchmarks/conference-scheduling/dump_instances.py <outdir>

Each line it prints ends with the score of the fixed assignment ``talk i -> slotRoom
(7 * i) % slotRoomCount`` under the use case's own scorer; ``cs_quality_bench <file> <seed> 0``
prints the C++ score of the same assignment, which must be equal.

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

import sys
from pathlib import Path

from conference_scheduling_cpsat import benchmark
from optimaru import conference_scheduling
from optimaru.conference_scheduling.models import ConferenceScheduleModel

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


def crosscheck(problem) -> str:
    """The score of the fixed assignment the C++ bench scores with a budget of 0 seconds.
    Scores a copy; a pinned talk keeps its place."""
    schedule = ConferenceScheduleModel.from_core(problem).to_core()
    cells = schedule.slot_room_count()
    current = schedule.assignment()
    pinned = [talk.pinned for talk in schedule.talks]
    schedule.set_assignment([current[i] if pinned[i] else (7 * i) % cells for i in range(len(current))])
    score = conference_scheduling.calculate_score(schedule)
    return f"hard={score.hard} soft={score.soft:.0f}"


def main(argv: list[str]) -> int:
    outdir = Path(argv[1]) if len(argv) > 1 else Path("out")
    outdir.mkdir(parents=True, exist_ok=True)
    for name, factory in benchmark.instances().items():
        problem = factory()
        path = outdir / f"{name}.cstxt"
        path.write_text(to_text(ConferenceScheduleModel.from_core(problem)), encoding="utf-8")
        print(f"{name}: {benchmark.describe(problem)} -> {path} (crosscheck {crosscheck(problem)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
