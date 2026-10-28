"""Track-1 CP-SAT solver: solves the quickstart's six constraints with OR-Tools CP-SAT.

Reads the instance JSON dumped by the C++ `st_quality_bench` and looks for a
lesson → (timeslot, room) assignment. The score scale is the quickstart's six
constraints, shared by all three solvers:

- the 3 hard ones (roomConflict/teacherConflict/studentGroupConflict) are CP-SAT
  hard constraints → any solution found has hard == 0.
- the 3 soft ones are the objective: maximize efficiency - stability - variety.
  * teacherRoomStability: per teacher, the lesson pairs in different rooms. Each
    pair reifies same_room[i][j] ↔ (room_i == room_j): all pairs - same-room pairs.
  * teacherTimeEfficiency: pairs of adjacent periods (same day, Δperiod == 1)
    a teacher occupies. Teacher×slot occupancy is 0/1 thanks to the hard
    constraints, so an adjacent pair is an AND.
  * studentGroupSubjectVariety: pairs of adjacent periods a (class, subject) occupies.

The output uses the same scale as C++/Java,
"QBENCH_CPSAT_FINAL score=Xhard/Ysoft" (+ the best trajectory / first_feasible),
and the solution is dumped to <instance name>.solution_cpsat.json for
evaluate.py's rescoring.

Usage:
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
    """Prints the best trajectory and the first-feasible time in the same format as C++/Java."""

    def __init__(self, t0: float, offset: int) -> None:
        super().__init__()
        self._t0 = t0
        self._offset = offset  # the objective's constant term (all pairs per teacher)
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
    """Solves the instance and returns (assignment, status string, metadata)."""
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

    # hard 1 roomConflict: one lesson per (timeslot, room) cell. AllDifferent over
    # the composite cell value (the same as the C++ slotRoom encoding).
    cell = []
    for i in range(len(lessons)):
        c = model.new_int_var(0, slot_count * room_count - 1, f"cell_{i}")
        model.add(c == slot[i] * room_count + room[i])
        cell.append(c)
    model.add_all_different(cell)

    # hard 2 + 3: lessons of the same teacher/class take different timeslots.
    for members in by_teacher.values():
        if len(members) > 1:
            model.add_all_different([slot[i] for i in members])
    for members in by_group.values():
        if len(members) > 1:
            model.add_all_different([slot[i] for i in members])

    # x[i][s] ↔ (slot_i == s): occupancy indicators for the soft constraints.
    x = {}
    for i in range(len(lessons)):
        for s in range(slot_count):
            b = model.new_bool_var(f"x_{i}_{s}")
            model.add(slot[i] == s).only_enforce_if(b)
            model.add(slot[i] != s).only_enforce_if(~b)
            x[i, s] = b

    objective: list = []
    offset = 0  # constant term: all pairs per teacher (stability = constant - same-room pairs)

    # soft: teacherRoomStability, flipped into maximize Σ same_room.
    for members in by_teacher.values():
        offset += len(members) * (len(members) - 1) // 2
        for a in range(len(members)):
            for b in range(a + 1, len(members)):
                i, j = members[a], members[b]
                same = model.new_bool_var(f"same_{i}_{j}")
                model.add(room[i] == room[j]).only_enforce_if(same)
                model.add(room[i] != room[j]).only_enforce_if(~same)
                objective.append(same)

    # soft: teacherTimeEfficiency (a reward). Teacher×slot occupancy is 0/1 thanks to the
    # hard constraints → an adjacent pair is an AND. As a reward only the upper bound (≤) is needed; the solver pushes it up.
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

    # soft: studentGroupSubjectVariety (a penalty). As a penalty only the lower bound (≥) is needed.
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
    parser.add_argument("instance", help="instance JSON dumped by st_quality_bench")
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

    # Reports the value rescored by the shared evaluator instead of its own objective (self-check).
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
