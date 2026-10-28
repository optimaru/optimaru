"""트랙1 CP-SAT 솔버: quickstart 6제약을 OR-Tools CP-SAT 로 푼다.

C++ `st_quality_bench` 가 덤프한 인스턴스 JSON 을 읽어 lesson → (timeslot, room)
배정을 찾는다. 스코어 척도는 세 솔버가 공유하는 quickstart 6제약 그대로:

- hard 3종(roomConflict/teacherConflict/studentGroupConflict)은 CP-SAT 의
  하드 제약으로 강제한다 → 해가 나오면 항상 hard == 0.
- soft 3종은 목적함수: maximize efficiency - stability - variety.
  * teacherRoomStability: 교사별 수업 쌍 중 다른 교실 쌍 수. 쌍마다
    same_room[i][j] ↔ (room_i == room_j) 를 reify 해서 전체쌍 - 같은교실쌍.
  * teacherTimeEfficiency: 교사가 인접 교시(같은 날 Δperiod == 1)를 점유한
    쌍 수. 교사×슬롯 점유는 하드 제약 덕에 0/1 이라 인접 쌍 = AND.
  * studentGroupSubjectVariety: (학급, 과목)이 인접 교시를 점유한 쌍 수.

출력 형식은 C++/Java 와 같은 척도의
"QBENCH_CPSAT_FINAL score=Xhard/Ysoft" (+ best 궤적 / first_feasible) 이고,
해는 <instance 이름>.solution_cpsat.json 으로 덤프해 evaluate.py 재채점에 쓴다.

사용:
    uv run cpsat_quickstart.py <instance.json> <seconds> [--seed N] [--workers N]
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from ortools.sat.python import cp_model

from evaluate import format_score, load_json, score_assignment

__all__ = ["solve_instance"]


def _next_slot_table(timeslots: list[dict]) -> list[int]:
    index_of = {(ts["day"], ts["period"]): i for i, ts in enumerate(timeslots)}
    return [index_of.get((ts["day"], ts["period"] + 1), -1) for ts in timeslots]


class _Progress(cp_model.CpSolverSolutionCallback):
    """best 궤적과 최초 feasible 시각을 C++/Java 와 같은 형식으로 찍는다."""

    def __init__(self, t0: float, offset: int) -> None:
        super().__init__()
        self._t0 = t0
        self._offset = offset  # 목적함수의 상수항 (교사별 전체 쌍 수)
        self.first_feasible_ms: int | None = None

    def on_solution_callback(self) -> None:
        ms = int((time.perf_counter() - self._t0) * 1000)
        if self.first_feasible_ms is None:
            self.first_feasible_ms = ms
            print(f"QBENCH_CPSAT first_feasible t={ms}ms")
        soft = int(self.objective_value) - self._offset
        print(f"QBENCH_CPSAT best t={ms}ms score=0hard/{soft}soft", flush=True)


def solve_instance(
    instance: dict, seconds: float, *, seed: int = 0, workers: int = 8
) -> tuple[list[tuple[int, int]] | None, str, dict]:
    """인스턴스를 풀어 (assignment, status 문자열, 메타) 를 돌려준다."""
    timeslots = instance["timeslots"]
    rooms = instance["rooms"]
    lessons = instance["lessons"]
    slot_count = len(timeslots)
    room_count = len(rooms)
    next_slot = _next_slot_table(timeslots)

    by_teacher: dict[str, list[int]] = {}
    by_group: dict[str, list[int]] = {}
    by_group_subject: dict[tuple[str, str], list[int]] = {}
    for i, lesson in enumerate(lessons):
        by_teacher.setdefault(lesson["teacher"], []).append(i)
        by_group.setdefault(lesson["studentGroup"], []).append(i)
        by_group_subject.setdefault((lesson["studentGroup"], lesson["subject"]), []).append(i)

    model = cp_model.CpModel()
    slot = [model.new_int_var(0, slot_count - 1, f"slot_{i}") for i in range(len(lessons))]
    room = [model.new_int_var(0, room_count - 1, f"room_{i}") for i in range(len(lessons))]

    # hard 1 roomConflict: (timeslot, room) 셀은 수업 하나만. 합성 셀 값으로
    # AllDifferent (C++ 의 slotRoom 인코딩과 동일).
    cell = []
    for i in range(len(lessons)):
        c = model.new_int_var(0, slot_count * room_count - 1, f"cell_{i}")
        model.add(c == slot[i] * room_count + room[i])
        cell.append(c)
    model.add_all_different(cell)

    # hard 2 + 3: 같은 교사/학급의 수업은 서로 다른 timeslot.
    for members in by_teacher.values():
        if len(members) > 1:
            model.add_all_different([slot[i] for i in members])
    for members in by_group.values():
        if len(members) > 1:
            model.add_all_different([slot[i] for i in members])

    # x[i][s] ↔ (slot_i == s): soft 제약의 점유 지표.
    x = {}
    for i in range(len(lessons)):
        for s in range(slot_count):
            b = model.new_bool_var(f"x_{i}_{s}")
            model.add(slot[i] == s).only_enforce_if(b)
            model.add(slot[i] != s).only_enforce_if(~b)
            x[i, s] = b

    objective: list = []
    offset = 0  # 상수항: 교사별 전체 쌍 수 (stability = 상수 - 같은교실쌍)

    # soft: teacherRoomStability. maximize Σ same_room 으로 뒤집는다.
    for members in by_teacher.values():
        offset += len(members) * (len(members) - 1) // 2
        for a in range(len(members)):
            for b in range(a + 1, len(members)):
                i, j = members[a], members[b]
                same = model.new_bool_var(f"same_{i}_{j}")
                model.add(room[i] == room[j]).only_enforce_if(same)
                model.add(room[i] != room[j]).only_enforce_if(~same)
                objective.append(same)

    # soft: teacherTimeEfficiency (보상). 하드 제약 때문에 교사×슬롯 점유는
    # 0/1 → 인접 쌍은 AND. 보상이므로 상한(≤)만 걸면 솔버가 끌어올린다.
    for teacher, members in by_teacher.items():
        for s in range(slot_count):
            nxt = next_slot[s]
            if nxt < 0:
                continue
            occ_here = sum(x[i, s] for i in members)
            occ_next = sum(x[i, nxt] for i in members)
            adj = model.new_bool_var(f"eff_{teacher}_{s}")
            model.add(adj <= occ_here)
            model.add(adj <= occ_next)
            objective.append(adj)

    # soft: studentGroupSubjectVariety (벌점). 벌점이므로 하한(≥)만 건다.
    for (group, subject), members in by_group_subject.items():
        for s in range(slot_count):
            nxt = next_slot[s]
            if nxt < 0:
                continue
            occ_here = sum(x[i, s] for i in members)
            occ_next = sum(x[i, nxt] for i in members)
            pen = model.new_bool_var(f"var_{group}_{subject}_{s}")
            model.add(pen >= occ_here + occ_next - 1)
            objective.append(-pen)

    model.maximize(sum(objective))

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = seconds
    solver.parameters.random_seed = seed
    solver.parameters.num_workers = workers

    t0 = time.perf_counter()
    progress = _Progress(t0, offset)
    status = solver.solve(model, progress)
    wall = time.perf_counter() - t0

    status_name = solver.status_name(status)
    meta = {
        "wall_time_seconds": wall,
        "first_feasible_ms": progress.first_feasible_ms,
        "status": status_name,
    }
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        return None, status_name, meta
    assignment = [(solver.value(slot[i]), solver.value(room[i])) for i in range(len(lessons))]
    return assignment, status_name, meta


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("instance", help="st_quality_bench 가 덤프한 인스턴스 JSON")
    parser.add_argument("seconds", type=float)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--workers", type=int, default=8)
    args = parser.parse_args()

    instance = load_json(args.instance)
    print(
        f"QBENCH_CPSAT instance={Path(args.instance).name}"
        f" lessons={len(instance['lessons'])} timeslots={len(instance['timeslots'])}"
        f" rooms={len(instance['rooms'])} workers={args.workers}"
    )
    assignment, status, meta = solve_instance(instance, args.seconds, seed=args.seed, workers=args.workers)
    if assignment is None:
        print(f"QBENCH_CPSAT_FINAL score=n/a status={status}")
        return 1

    # 자기 목적함수 값 대신 공유 평가기로 재채점한 값을 보고한다 (셀프체크).
    result = score_assignment(instance, assignment)
    out_path = Path(args.instance).with_suffix("").as_posix() + ".solution_cpsat.json"
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(
            {
                "solver": "cpsat",
                "score": format_score(result),
                "status": status,
                "assignment": [
                    {"id": lesson["id"], "timeslot": int(ts), "room": int(r)}
                    for lesson, (ts, r) in zip(instance["lessons"], assignment)
                ],
            },
            f,
            indent=2,
        )
    print(f"QBENCH_CPSAT_FINAL score={format_score(result)} status={status} wall={meta['wall_time_seconds']:.1f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
