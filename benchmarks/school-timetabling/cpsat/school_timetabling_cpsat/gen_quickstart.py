"""Track-1 instance generator: the quickstart model's schools, and the fixed cross-check instance.

An instance is a 5 days x 8 periods week (40 timeslots), a fixed 34-period weekly curriculum
per class and one teacher per (class, subject). Subject ``s`` is taught by the teacher pool
``{t : t % len(CURRICULUM) == s}``; the classes are shuffled with the seed and dealt
round-robin over the pool, so the heaviest teacher carries ``ceil(classes / pool) * hours``
periods. ``generate`` refuses a school in which that exceeds the 40-slot week or the lessons
exceed the cells (40 x rooms), so every school it writes leaves each teacher and class a
conflict-free week in principle; the presets are tight on rooms (73-82% of the cells). ``teacherUnavailable`` is not part of the format, so the model's seventh term,
teacher_unavailability_cost, contributes 0.

Every draw comes from ``random.Random(seed)``, so a seed is the same school on every
platform.

``write`` stores an instance twice, from one dict:

- ``<name>.json``, read by ``cpsat_quickstart`` and ``evaluate``;
- ``<name>.sttxt``, the same data as whitespace-separated tokens, read by the C++ bench
  (``benchmarks/school-timetabling/cpp/st_quality_bench.cpp``), which has no JSON parser::

      ST1
      timeslots <n>
        <day> <period>
      rooms <n>
        <name>
      lessons <n>
        <id> <subject> <teacher> <studentGroup> <timeslot|-1> <room|-1>

  Every name is a single token (the writer checks).

Usage (from benchmarks/school-timetabling/cpsat):
    uv run python -m school_timetabling_cpsat.gen_quickstart --sizes S,M,L --seed 1 --out-dir out
    uv run python -m school_timetabling_cpsat.gen_quickstart --sizes "" --school 28 47 30   # any shape
"""

from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

__all__ = ["CURRICULUM", "PRESETS", "crosscheck", "generate", "to_text", "write"]

DAYS = 5
PERIODS_PER_DAY = 8

# Weekly hours per class; they sum to 34 (6 free cells per class in the 40-slot week).
CURRICULUM: tuple[tuple[str, int], ...] = (
    ("KOR", 5),
    ("MATH", 5),
    ("ENG", 4),
    ("SCI", 4),
    ("SOC", 4),
    ("PE", 3),
    ("MUS", 3),
    ("ART", 2),
    ("ETH", 2),
    ("HIS", 2),
)

# Size presets: (classes, teachers, rooms).
#   S:  6 12  7   (204 lessons, 40 slots x  7 rooms =  280 cells, 73%)
#   M: 12 20 13   (408 lessons, 40 slots x 13 rooms =  520 cells, 78%)
#   L: 24 40 25   (816 lessons, 40 slots x 25 rooms = 1000 cells, 82%)
PRESETS: dict[str, tuple[int, int, int]] = {
    "S": (6, 12, 7),
    "M": (12, 20, 13),
    "L": (24, 40, 25),
}


def _instance(days: int, periods: int, rooms: list[str], lessons: list[dict]) -> dict:
    return {
        "days": days,
        "periodsPerDay": periods,
        "timeslots": [{"day": day, "period": period} for day in range(days) for period in range(1, periods + 1)],
        "rooms": rooms,
        "lessons": lessons,
    }


def generate(classes: int, teachers: int, rooms: int, seed: int) -> dict:
    """A school of ``classes`` classes, ``teachers`` teachers and ``rooms`` rooms, unassigned."""
    if classes < 1 or rooms < 1:
        raise ValueError(f"need at least one class and one room, got {classes} and {rooms}")
    if teachers < len(CURRICULUM):
        raise ValueError(f"need at least {len(CURRICULUM)} teachers (one per subject), got {teachers}")
    week = DAYS * PERIODS_PER_DAY
    lessons_per_class = sum(hours for _, hours in CURRICULUM)
    if classes * lessons_per_class > week * rooms:
        raise ValueError(
            f"{classes} classes need {classes * lessons_per_class} lessons, more than the {week * rooms} cells of "
            f"{rooms} rooms"
        )
    for subject_index, (subject, hours) in enumerate(CURRICULUM):
        pool = len(range(subject_index, teachers, len(CURRICULUM)))
        if -(-classes // pool) * hours > week:
            raise ValueError(
                f"{subject}: {pool} teacher(s) for {classes} classes carry {-(-classes // pool) * hours} periods, "
                f"more than the {week}-slot week"
            )
    rng = random.Random(seed)
    groups = [f"C{index + 1:02d}" for index in range(classes)]

    # teacher_of[subject index][class index]
    teacher_of: list[list[str]] = []
    for subject in range(len(CURRICULUM)):
        pool = list(range(subject, teachers, len(CURRICULUM)))
        order = list(range(classes))
        rng.shuffle(order)
        assigned = [""] * classes
        for position, group in enumerate(order):
            assigned[group] = f"T{pool[position % len(pool)]}"
        teacher_of.append(assigned)

    lessons = [
        {
            "id": f"{group}-{subject}-{hour}",
            "subject": subject,
            "teacher": teacher_of[subject_index][group_index],
            "studentGroup": group,
            "timeslot": -1,
            "room": -1,
        }
        for group_index, group in enumerate(groups)
        for subject_index, (subject, hours) in enumerate(CURRICULUM)
        for hour in range(1, hours + 1)
    ]
    return _instance(DAYS, PERIODS_PER_DAY, [f"R{index}" for index in range(rooms)], lessons)


def crosscheck() -> dict:
    """The fixed cross-check instance: 6 timeslots (2 days x 3 periods), 2 rooms and 7 lessons
    whose fixed assignment makes each of the six constraints contribute something non-zero.

    hard = -(1 room + 1 teacher + 1 class conflict) = -3; soft = teacher_time_efficiency_cost 3
    - teacher_room_change_cost 4 - class_subject_repeat_cost 2 = -3.
    """
    fixed = [
        ("L0", "MATH", "TA", "G1", 0, 0),
        ("L1", "MATH", "TA", "G1", 1, 0),
        ("L2", "PHYS", "TB", "G2", 0, 0),  # room conflict with L0
        ("L3", "PHYS", "TB", "G1", 0, 1),  # teacher (L2) + class (L0) conflict
        ("L4", "CHEM", "TB", "G2", 1, 1),
        ("L5", "MATH", "TA", "G2", 5, 1),
        ("L6", "MATH", "TC", "G2", 4, 0),
    ]
    lessons = [
        {"id": lesson, "subject": subject, "teacher": teacher, "studentGroup": group, "timeslot": slot, "room": room}
        for lesson, subject, teacher, group, slot, room in fixed
    ]
    return _instance(2, 3, ["R0", "R1"], lessons)


def _token(text: str) -> str:
    if not text or any(ch.isspace() for ch in text):
        raise ValueError(f"not a single token: {text!r}")
    return text


def to_text(instance: dict) -> str:
    """The instance as the token file the C++ bench reads (see the module docstring)."""
    lines = ["ST1", f"timeslots {len(instance['timeslots'])}"]
    lines += [f"{slot['day']} {slot['period']}" for slot in instance["timeslots"]]
    lines.append(f"rooms {len(instance['rooms'])}")
    lines += [_token(room) for room in instance["rooms"]]
    lines.append(f"lessons {len(instance['lessons'])}")
    for lesson in instance["lessons"]:
        names = [_token(lesson[key]) for key in ("id", "subject", "teacher", "studentGroup")]
        lines.append(" ".join([*names, str(lesson["timeslot"]), str(lesson["room"])]))
    return "\n".join(lines) + "\n"


def write(instance: dict, json_path: str | Path) -> Path:
    """Writes ``json_path`` and, next to it, the same instance as ``.sttxt``. Returns the latter."""
    json_path = Path(json_path)
    json_path.write_text(json.dumps(instance, indent=2) + "\n", encoding="utf-8")
    text_path = json_path.with_suffix(".sttxt")
    text_path.write_text(to_text(instance), encoding="utf-8")
    return text_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sizes", default="S,M,L", help='comma-separated presets: S,M,L ("" for none)')
    parser.add_argument(
        "--school",
        type=int,
        nargs=3,
        action="append",
        default=[],
        metavar=("CLASSES", "TEACHERS", "ROOMS"),
        help="a school of any shape, written as instance_<classes>x<teachers>x<rooms> (repeatable)",
    )
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out-dir", default="out")
    args = parser.parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    sizes = [size for size in (s.strip().upper() for s in args.sizes.split(",")) if size]
    if unknown := [size for size in sizes if size not in PRESETS]:
        parser.error(f"--sizes: unknown preset(s) {unknown}; the presets are {sorted(PRESETS)}")
    schools = [(size, PRESETS[size]) for size in sizes]
    schools += [("x".join(str(n) for n in shape), tuple(shape)) for shape in args.school]
    for name, (classes, teachers, rooms) in schools:
        try:
            instance = generate(classes, teachers, rooms, args.seed)
        except ValueError as error:
            parser.error(f"--school {classes} {teachers} {rooms}: {error}")
        path = out_dir / f"instance_{name}.json"
        text_path = write(instance, path)
        print(
            f"{name}: {classes} classes / {teachers} teachers / {rooms} rooms, "
            f"{len(instance['lessons'])} lessons -> {path}, {text_path}"
        )
    path = out_dir / "crosscheck.json"
    print(f"crosscheck -> {path}, {write(crosscheck(), path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
