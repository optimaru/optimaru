"""트랙1 공유 평가기: quickstart 6제약 스코어를 인스턴스 JSON + 할당에서 재계산.

세 솔버(C++ 엔진 / Java Timefold / CP-SAT)의 해를 전부 같은 함수로 재채점해서
각 솔버의 self-report 점수와 대조한다. 계산 규칙은 Java quickstart 의
``TimetableConstraintProvider`` (= C++ ``score_calculator.cpp``)와 동일하다:

- hard: roomConflict / teacherConflict / studentGroupConflict —
  같은 키 셀을 공유하는 수업 쌍 수 C(c, 2) 의 합에 -1 배.
- soft: teacherTimeEfficiency(보상) - teacherRoomStability(벌점)
  - studentGroupSubjectVariety(벌점). "연속 교시" 는 같은 날 |Δperiod| == 1
  (Java 쪽은 시각 매핑으로 같은 의미가 되도록 크로스체크로 고정).

사용:
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
    """``assignment[i] = (timeslot index, room index)`` (미배정은 (-1, -1)) 를
    quickstart 6제약으로 채점한다. 반환 dict 은 hard/soft 와 제약별 내역."""
    timeslots = instance["timeslots"]
    lessons = instance["lessons"]
    if len(assignment) != len(lessons):
        raise ValueError(f"assignment 길이 {len(assignment)} != lessons {len(lessons)}")

    # 같은 날 period+1 인접 timeslot (C++ 의 nextSlot 과 동일).
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
            continue  # 미배정 수업은 아무 기여도 없다 (Java forEach 와 동일)
        cell_cnt[(ts, room)] += 1
        teacher_slot[(lesson["teacher"], ts)] += 1
        group_slot[(lesson["studentGroup"], ts)] += 1
        group_subject_slot[(lesson["studentGroup"], lesson["subject"], ts)] += 1
        teacher_room[(lesson["teacher"], room)] += 1
        teacher_lessons[lesson["teacher"]] += 1

    room_conflict = sum(_pairs(c) for c in cell_cnt.values())
    teacher_conflict = sum(_pairs(c) for c in teacher_slot.values())
    group_conflict = sum(_pairs(c) for c in group_slot.values())

    # teacherRoomStability: 교사별 전체 쌍 - 같은 교실 쌍.
    same_room_pairs: Counter[str] = Counter()
    for (teacher, _), c in teacher_room.items():
        same_room_pairs[teacher] += _pairs(c)
    stability = sum(_pairs(n) - same_room_pairs[t] for t, n in teacher_lessons.items())

    # 연속 교시 쌍 = 인접 셀 점유 수의 곱 (쌍마다 1회).
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
    """C++ ``HardSoftScore::toString`` / Java ``HardSoftScore#toString`` 과 같은
    "Xhard/Ysoft" 문자열."""
    return f"{result['hard']}hard/{result['soft']}soft"


def assignment_from_solution(instance: dict, solution: dict) -> list[tuple[int, int]]:
    """solution JSON({"assignment": [{"id", "timeslot", "room"}, ...]}) 을 인스턴스의
    lesson 순서에 맞춘 (timeslot, room) 리스트로 바꾼다. id 로 매칭한다."""
    by_id = {entry["id"]: (entry["timeslot"], entry["room"]) for entry in solution["assignment"]}
    missing = [lesson["id"] for lesson in instance["lessons"] if lesson["id"] not in by_id]
    if missing:
        raise ValueError(f"solution 에 없는 lesson id: {missing[:5]} ...")
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
