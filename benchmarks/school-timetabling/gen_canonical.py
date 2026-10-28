"""Track-2 canonical instance generator (S/M/L).

Builds canonical `TimetableProblem` JSON (API_SPEC.md's serialized shape) by
scaling up the `teacher_scheduling.sample` demo school of
benchmarks/school-timetabling/cpsat.

- 5-day week: Mon–Thu 7 periods, Fri 6 periods → 34 cells per class
- Classes: S=6 (3 grades × 2) / M=12 (3 grades × 4) / L=24 (3 grades × 8)
- Subject periods total 33 + homeroom (Monday period 1, fixed, homeroom teacher) 1 = 34
  → the timetable fills up with no empty cell
- Special rooms (science lab / music room / art room / gym) grow with the class count
- Art once a week as a 2-period block (block_periods=2)
- Parallel sections: each grade's class 1 splits technology/home economics into two groups (parallel_group)
- Team teaching: one English hour of each grade's class 1 has two teachers at once
- Unavailability: training/other-school slots for some teachers, plus some requested free days (soft)

Load check: each class's weekly periods == available slots, each teacher's
weekly load ≤ min(30, max_periods_per_week); where possible the result is also
validated with teacher_scheduling's pydantic models.

Usage:
    uv run gen_canonical.py --sizes S,M,L --out-dir out
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

# Used for the final validation with the canonical pydantic models of benchmarks/school-timetabling/cpsat.
_CPSAT_DIR = Path(__file__).resolve().parent / "cpsat"

# Class-count presets.
SIZES = {"S": 6, "M": 12, "L": 24}

# Subject periods (homeroom hour separate). Total 33.
CURRICULUM: dict[str, int] = {
    "KOR": 5,
    "MATH": 5,
    "ENG": 5,
    "SCI": 4,
    "SOC": 4,
    "PE": 3,
    "MUS": 2,
    "ART": 2,
    "TECH": 2,
    "ETH": 1,
}

# Maximum classes one teacher takes per subject (keeps the weekly load under 30 hours).
_CHUNK: dict[str, int] = {
    "KOR": 4,
    "MATH": 4,
    "ENG": 4,
    "SCI": 5,
    "SOC": 4,  # the same teacher also teaches ETH (social + ethics = 5 hours per class)
    "PE": 8,
    "MUS": 10,
    "ART": 10,
    "TECH": 6,
}

_SUBJECT_NAMES = {
    "KOR": "Korean",
    "MATH": "Math",
    "ENG": "English",
    "SCI": "Science",
    "SOC": "Social studies",
    "PE": "PE",
    "MUS": "Music",
    "ART": "Art",
    "TECH": "Technology and Home Economics",
    "ETH": "Ethics",
    "HR": "Homeroom",
}

# Periods per weekday (Weekday 0=Mon .. 4=Fri).
_PERIODS_PER_DAY = {0: 7, 1: 7, 2: 7, 3: 7, 4: 6}
_SLOTS_PER_WEEK = sum(_PERIODS_PER_DAY.values())  # 34


def _class_ids(classes: int) -> list[str]:
    """Spread evenly over 3 grades: C11, C12, ..., C21, ..."""
    per_grade = classes // 3
    return [f"C{grade}{section}" for grade in (1, 2, 3) for section in range(1, per_grade + 1)]


def generate(size: str) -> dict:
    classes = SIZES[size]
    if classes % 3 != 0:
        raise ValueError("the class count must be a multiple of 3 (spread evenly over 3 grades)")
    class_ids = _class_ids(classes)

    # --- teachers: per subject, classes are dealt out in chunks ---
    teachers: list[dict] = []
    teacher_of: dict[tuple[str, str], str] = {}  # (subject, class) -> teacher id
    pools: dict[str, list[str]] = defaultdict(list)

    def new_teacher(subject_ids: list[str]) -> str:
        tid = f"T{len(teachers) + 1:02d}"
        teachers.append(
            {
                "id": tid,
                "name": f"{_SUBJECT_NAMES[subject_ids[0]]}{len(pools[subject_ids[0]]) + 1}",
                "subject_ids": subject_ids,
                "unavailable": [],
                "max_periods_per_week": None,
                "max_periods_per_day": 6,
                "max_consecutive_periods": 4,
                "preferred_free_days": [],
                "homeroom_class_id": None,
            }
        )
        for s in subject_ids:
            pools[s].append(tid)
        return tid

    for subject, chunk in _CHUNK.items():
        subject_ids = ["SOC", "ETH"] if subject == "SOC" else [subject]
        count = max(2 if subject in ("TECH", "ENG") else 1, math.ceil(classes / chunk))
        pool = [new_teacher(subject_ids) for _ in range(count)]
        for i, cid in enumerate(class_ids):
            teacher_of[subject, cid] = pool[i % len(pool)]
            if subject == "SOC":
                teacher_of["ETH", cid] = pool[i % len(pool)]

    # Homeroom teachers: one class per teacher from the front, in charge of homeroom (HR).
    by_id = {t["id"]: t for t in teachers}
    if len(teachers) < classes:
        raise ValueError(f"cannot assign homeroom teachers: {len(teachers)} teachers < {classes} classes")
    for i, cid in enumerate(class_ids):
        teacher = teachers[i]
        teacher["homeroom_class_id"] = cid
        teacher["subject_ids"] = [*teacher["subject_ids"], "HR"]

    # Unavailability / requested free days: every 4th teacher has training in period 7 on
    # Mon/Tue, every 6th requests Friday off (soft). Music teacher 1 teaches at another school all Friday.
    for i, teacher in enumerate(teachers):
        if i % 4 == 3:
            teacher["unavailable"] = [{"day": 0, "period": 7}, {"day": 1, "period": 7}]
        if i % 6 == 5:
            teacher["preferred_free_days"] = [4]
    mus1 = by_id[pools["MUS"][0]]
    mus1["unavailable"] = [{"day": 4, "period": p} for p in range(1, _PERIODS_PER_DAY[4] + 1)]

    # --- special rooms: in proportion to the class count ---
    rooms: list[dict] = []
    for room_type, korean, count in (
        ("SCIENCE_LAB", "Science lab", max(2, math.ceil(classes / 3))),
        ("MUSIC_ROOM", "Music room", max(1, math.ceil(classes / 8))),
        ("ART_ROOM", "Art room", max(1, math.ceil(classes / 8))),
        ("GYM", "Gym", max(1, math.ceil(classes / 6))),
    ):
        for k in range(1, count + 1):
            rooms.append({"id": f"R_{room_type}{k}", "name": f"{korean}{k}", "room_type": room_type})

    subjects = [
        {"id": "KOR", "name": "Korean", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {
            "id": "MATH",
            "name": "Math",
            "required_room_type": None,
            "preferred_periods": [1, 2, 3, 4],
            "avoided_periods": [],
        },
        {"id": "ENG", "name": "English", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {
            "id": "SCI",
            "name": "Science",
            "required_room_type": "SCIENCE_LAB",
            "preferred_periods": [],
            "avoided_periods": [],
        },
        {"id": "SOC", "name": "Social studies", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {"id": "PE", "name": "PE", "required_room_type": "GYM", "preferred_periods": [], "avoided_periods": [1]},
        {
            "id": "MUS",
            "name": "Music",
            "required_room_type": "MUSIC_ROOM",
            "preferred_periods": [],
            "avoided_periods": [],
        },
        {"id": "ART", "name": "Art", "required_room_type": "ART_ROOM", "preferred_periods": [], "avoided_periods": []},
        {"id": "TECH", "name": "Technology and Home Economics", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {"id": "ETH", "name": "Ethics", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {"id": "HR", "name": "Homeroom", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
    ]

    # --- courses ---
    def course(cid: str, subject: str, teacher_ids: list[str], periods: int, **extra) -> dict:
        return {
            "id": extra.pop("id", f"{cid}-{subject}"),
            "class_group_id": cid,
            "subject_id": subject,
            "teacher_ids": teacher_ids,
            "periods_per_week": periods,
            "block_periods": extra.pop("block_periods", 0),
            "max_periods_per_day": extra.pop("max_periods_per_day", 1),
            "fixed_slots": extra.pop("fixed_slots", []),
            "forbidden_slots": [],
            "parallel_group": extra.pop("parallel_group", None),
        }

    split_classes = {f"C{grade}1" for grade in (1, 2, 3)}  # each grade's class 1: parallel sections + team teaching
    courses: list[dict] = []
    for cid in class_ids:
        for subject, hours in CURRICULUM.items():
            tid = teacher_of[subject, cid]
            if subject == "TECH" and cid in split_classes:
                # Parallel sections: technology and home economics groups at the same time (two teachers needed).
                pool = pools["TECH"]
                other = pool[(pool.index(tid) + 1) % len(pool)]
                for suffix, teacher in (("A", tid), ("B", other)):
                    courses.append(
                        course(cid, "TECH", [teacher], hours, id=f"{cid}-TECH-{suffix}", parallel_group=f"{cid}-TECH")
                    )
                continue
            if subject == "ENG" and cid in split_classes:
                # Team teaching: one English hour with two teachers (the main course loses one hour).
                pool = pools["ENG"]
                other = pool[(pool.index(tid) + 1) % len(pool)]
                courses.append(course(cid, "ENG", [tid], hours - 1))
                courses.append(course(cid, "ENG", [tid, other], 1, id=f"{cid}-ENG-TT"))
                continue
            if subject == "ART":
                # Art once a week as a 2-period block.
                courses.append(course(cid, "ART", [tid], hours, block_periods=2))
                continue
            courses.append(course(cid, subject, [tid], hours))
        # Homeroom: the homeroom teacher, fixed to Monday period 1.
        homeroom = next(t["id"] for t in teachers if t["homeroom_class_id"] == cid)
        courses.append(course(cid, "HR", [homeroom], 1, id=f"{cid}-HR", fixed_slots=[{"day": 0, "period": 1}]))

    problem = {
        "calendar": {"periods_per_day": {str(d): p for d, p in _PERIODS_PER_DAY.items()}},
        "subjects": subjects,
        "teachers": teachers,
        "class_groups": [
            {"id": cid, "name": f"{cid[1]}-{int(cid[2:])}", "grade": int(cid[1]), "unavailable": []}
            for cid in class_ids
        ],
        "rooms": rooms,
        "courses": courses,
    }
    _verify(problem, size)
    return problem


def _verify(problem: dict, size: str) -> None:
    """Load check: class weekly periods == available slots, teacher load ≤ cap."""
    per_class: dict[str, int] = defaultdict(int)
    per_teacher: dict[str, int] = defaultdict(int)
    parallel_seen: set[str] = set()
    for c in problem["courses"]:
        # A parallel group occupies the class cell once.
        key = c["parallel_group"]
        if key is None or key not in parallel_seen:
            per_class[c["class_group_id"]] += c["periods_per_week"]
            if key is not None:
                parallel_seen.add(key)
        for tid in c["teacher_ids"]:
            per_teacher[tid] += c["periods_per_week"]

    for cid, total in per_class.items():
        if total != _SLOTS_PER_WEEK:
            raise ValueError(f"[{size}] class {cid}: weekly periods {total} != slots {_SLOTS_PER_WEEK}")
    for teacher in problem["teachers"]:
        cap = min(30, teacher["max_periods_per_week"] or 30)
        unavailable = len(teacher["unavailable"])
        load = per_teacher[teacher["id"]]
        if load > cap - unavailable:
            raise ValueError(f"[{size}] teacher {teacher['id']}: {load} hours a week > cap {cap} - unavailable {unavailable}")

    # Where possible, validate with the canonical pydantic models too (referential integrity, parallel-group consistency, ...).
    if str(_CPSAT_DIR) not in sys.path:
        sys.path.insert(0, str(_CPSAT_DIR))
    try:
        from teacher_scheduling import TimetableProblem
    except ImportError:
        print(f"[{size}] teacher_scheduling not installed — skipping the pydantic validation", file=sys.stderr)
        return
    TimetableProblem.model_validate(problem)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", default="S,M,L", help="comma-separated: S,M,L")
    parser.add_argument("--out-dir", default="out")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for size in args.sizes.split(","):
        size = size.strip().upper()
        problem = generate(size)
        path = out_dir / f"canonical_{size}.json"
        with open(path, "w", encoding="utf-8") as f:
            json.dump(problem, f, ensure_ascii=False, indent=2)
        print(
            f"GEN canonical_{size}: classes={len(problem['class_groups'])}"
            f" teachers={len(problem['teachers'])} rooms={len(problem['rooms'])}"
            f" courses={len(problem['courses'])} -> {path}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
