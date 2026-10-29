"""Track-1 shared evaluator: recomputes the quickstart's six-constraint score from the instance JSON + an assignment.

Rescores the solutions of both solvers (C++ engine / CP-SAT) with the same
function and compares them with each solver's self-reported score. The rules
are identical to the Java quickstart's
``TimetableConstraintProvider`` (= C++ ``score_calculator.cpp``):

- hard: roomConflict / teacherConflict / studentGroupConflict —
  -1 times the sum of C(c, 2) over the lessons sharing a cell of that key.
- soft: teacherTimeEfficiency (reward) - teacherRoomStability (penalty)
  - studentGroupSubjectVariety (penalty). "Consecutive periods" means
  |Δperiod| == 1 on the same day.

Usage:
    uv run evaluate.py <instance.json> <solution.json>
"""

from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

__all__ = ["format_score", "load_json", "score_assignment"]


def load_json(path: str | Path) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _pairs(n: int) -> int:
    return n * (n - 1) // 2


def score_assignment(instance: dict, assignment: list[tuple[int, int]]) -> dict:
    """Scores ``assignment[i] = (timeslot index, room index)`` (unassigned is (-1, -1))
    with the quickstart's six constraints. Returns a dict with hard/soft and the per-constraint breakdown."""
    timeslots = instance["timeslots"]
    lessons = instance["lessons"]
    if len(assignment) != len(lessons):
        raise ValueError(f"assignment length {len(assignment)} != lessons {len(lessons)}")

    # The adjacent timeslot with period+1 on the same day (same as C++'s nextSlot).
    index_of = {(ts["day"], ts["period"]): i for i, ts in enumerate(timeslots)}
    next_slot = [index_of.get((ts["day"], ts["period"] + 1), -1) for ts in timeslots]

    cell_cnt: Counter[tuple[int, int]] = Counter()  # (timeslot, room)
    teacher_slot: Counter[tuple[str, int]] = Counter()
    group_slot: Counter[tuple[str, int]] = Counter()
    group_subject_slot: Counter[tuple[str, str, int]] = Counter()
    teacher_room: Counter[tuple[str, int]] = Counter()
    teacher_lessons: Counter[str] = Counter()

    for lesson, (ts, room) in zip(lessons, assignment):
        if ts < 0 or room < 0:
            continue  # an unassigned lesson contributes nothing (same as Java's forEach)
        cell_cnt[(ts, room)] += 1
        teacher_slot[(lesson["teacher"], ts)] += 1
        group_slot[(lesson["studentGroup"], ts)] += 1
        group_subject_slot[(lesson["studentGroup"], lesson["subject"], ts)] += 1
        teacher_room[(lesson["teacher"], room)] += 1
        teacher_lessons[lesson["teacher"]] += 1

    room_conflict = sum(_pairs(c) for c in cell_cnt.values())
    teacher_conflict = sum(_pairs(c) for c in teacher_slot.values())
    group_conflict = sum(_pairs(c) for c in group_slot.values())

    # teacherRoomStability: all pairs per teacher - same-room pairs.
    same_room_pairs: Counter[str] = Counter()
    for (teacher, _), c in teacher_room.items():
        same_room_pairs[teacher] += _pairs(c)
    stability = sum(_pairs(n) - same_room_pairs[t] for t, n in teacher_lessons.items())

    # Consecutive-period pairs = product of the occupancy counts of adjacent cells (once per pair).
    efficiency = sum(
        c * teacher_slot[(teacher, next_slot[ts])] for (teacher, ts), c in teacher_slot.items() if next_slot[ts] >= 0
    )
    variety = sum(
        c * group_subject_slot[(group, subject, next_slot[ts])]
        for (group, subject, ts), c in group_subject_slot.items()
        if next_slot[ts] >= 0
    )

    hard = -(room_conflict + teacher_conflict + group_conflict)
    soft = efficiency - stability - variety
    return {
        "hard": hard,
        "soft": soft,
        "room_conflict": room_conflict,
        "teacher_conflict": teacher_conflict,
        "group_conflict": group_conflict,
        "stability": stability,
        "efficiency": efficiency,
        "variety": variety,
    }


def format_score(result: dict) -> str:
    """The "Xhard/Ysoft" string, same as C++ ``HardSoftScore::toString`` / Java
    ``HardSoftScore#toString``."""
    return f"{result['hard']}hard/{result['soft']}soft"


def assignment_from_solution(instance: dict, solution: dict) -> list[tuple[int, int]]:
    """Turns a solution JSON ({"assignment": [{"id", "timeslot", "room"}, ...]}) into a
    (timeslot, room) list in the instance's lesson order, matched by id."""
    by_id = {entry["id"]: (entry["timeslot"], entry["room"]) for entry in solution["assignment"]}
    missing = [lesson["id"] for lesson in instance["lessons"] if lesson["id"] not in by_id]
    if missing:
        raise ValueError(f"lesson ids missing from the solution: {missing[:5]} ...")
    return [by_id[lesson["id"]] for lesson in instance["lessons"]]


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: evaluate.py <instance.json> <solution.json>", file=sys.stderr)
        return 2
    instance = load_json(argv[1])
    solution = load_json(argv[2])
    result = score_assignment(instance, assignment_from_solution(instance, solution))
    print(f"EVAL score={format_score(result)} self_report={solution.get('score', 'n/a')}")
    print(
        "EVAL breakdown"
        f" room_conflict={result['room_conflict']}"
        f" teacher_conflict={result['teacher_conflict']}"
        f" group_conflict={result['group_conflict']}"
        f" efficiency={result['efficiency']}"
        f" stability={result['stability']}"
        f" variety={result['variety']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
