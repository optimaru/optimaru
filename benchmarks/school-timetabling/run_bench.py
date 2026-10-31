"""Orchestrates the school-timetabling benchmark.

Track 1 (the quickstart's six constraints, default): the same instances are solved by
  (1) this repository's C++ engine (`st_quality_bench`),
  (2) OR-Tools CP-SAT (`cpsat_quickstart.py`),
and compared in a Markdown table (hard/soft, first-feasible time, moves/s).
Cross-check gate first: the C++ score of a fixed instance must match
evaluate.py before the benchmark proceeds. Every solution is rescored with
evaluate.py and compared with its self-report.

Track 2 (--track2, full canonical constraints): the S/M/L canonical problems of
gen_canonical.py are solved by CP-SAT (teacher_scheduling.solve) and by the C++
canonical engine (st_timetabling.solve; "pending" if the import fails), and
both results are rescored with cpsat's validate.py (same time limit, same problem).

Track 3 (--track3, partial timetabling): a track-2 instance loses one weekday
(--drop-day) so it cannot be filled, and both backends solve with H1 relaxed
(CP-SAT solve_partial, the engine with weekly_period_counts hard=false); both
are rescored with cpsat's validate.py, with H1 shortfall counted separately.

Example:
    uv run run_bench.py --sizes S,M --seconds 10
    uv run run_bench.py --track2 --sizes S,M,L --seconds 10
    uv run run_bench.py --track3 --sizes S,M --seconds 10
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

from evaluate import assignment_from_solution, format_score, load_json, score_assignment

BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parents[1]
ST_DIR = REPO_ROOT / "use-cases" / "school-timetabling"

# Track-1 size presets: (classes, teachers, rooms). Same as documented in st_quality_bench.cpp.
PRESETS: dict[str, tuple[int, int, int]] = {
    "S": (6, 12, 7),
    "M": (12, 20, 13),
    "L": (24, 40, 25),
}


def _run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    print(f"$ {' '.join(str(c) for c in cmd)}", flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, text=True, **kwargs)


def ensure_cpp_bin(explicit: str | None) -> Path:
    """Finds the st_quality_bench binary or builds it with cmake."""
    if explicit:
        return Path(explicit)
    build_dir = REPO_ROOT / "build" / "st-bench"
    binary = build_dir / "st_quality_bench"
    _run(["cmake", "-S", ST_DIR / "cpp", "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release"], stdout=subprocess.DEVNULL)
    _run(["cmake", "--build", build_dir, "--target", "st_quality_bench", "-j"], stdout=subprocess.DEVNULL)
    return binary


# ---------------------------------------------------------------- cross-check


def crosscheck_gate(cpp_bin: Path, out_dir: Path) -> None:
    """Parity gate between the C++ score calculator and evaluate.py."""
    crosscheck_json = out_dir / "crosscheck.json"
    proc = _run([cpp_bin, "crosscheck", crosscheck_json], capture_output=True)
    cpp_score = re.search(r"CROSSCHECK_SCORE=\S+", proc.stdout).group(0)
    print(f"C++ : {cpp_score}")

    # Rescore the dumped fixed assignment with evaluate.py.
    instance = load_json(crosscheck_json)
    eval_score = format_score(score_assignment(instance, [(l["timeslot"], l["room"]) for l in instance["lessons"]]))
    if cpp_score != f"CROSSCHECK_SCORE={eval_score}":
        raise SystemExit(f"evaluate.py disagrees: {eval_score}")
    print(f"eval: CROSSCHECK_SCORE={eval_score}")
    print("cross-check gate: OK")


# ---------------------------------------------------------------- track-1 runners


def run_cpp(cpp_bin: Path, size: str, seed: int, seconds: int, out_dir: Path) -> dict:
    classes, teachers, rooms = PRESETS[size]
    instance = out_dir / f"instance_{size}.json"
    proc = _run(
        [cpp_bin, classes, teachers, rooms, seed, seconds, instance], capture_output=True, timeout=seconds + 120
    )
    final = re.search(r"QBENCH_CPP_FINAL score=(\S+) feasible=\S+ moves_per_sec=(\d+)", proc.stdout)
    feasible = re.search(r"QBENCH_CPP first_feasible t=(\d+)ms", proc.stdout)
    return {
        "score": final.group(1),
        "moves_per_sec": int(final.group(2)),
        "first_feasible_ms": int(feasible.group(1)) if feasible else None,
        "solution": out_dir / f"instance_{size}.solution_cpp.json",
    }


def run_cpsat(size: str, seed: int, seconds: int, out_dir: Path, workers: int) -> dict:
    import cpsat_quickstart

    instance_path = out_dir / f"instance_{size}.json"
    instance = load_json(instance_path)
    assignment, status, meta = cpsat_quickstart.solve_instance(instance, float(seconds), seed=seed, workers=workers)
    solution_path = out_dir / f"instance_{size}.solution_cpsat.json"
    if assignment is None:
        return {"score": f"n/a ({status})", "moves_per_sec": None, "first_feasible_ms": None, "solution": None}
    result = score_assignment(instance, assignment)
    with open(solution_path, "w", encoding="utf-8") as f:
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
    return {
        "score": format_score(result),
        "moves_per_sec": None,  # CP-SAT has no notion of moves
        "first_feasible_ms": meta["first_feasible_ms"],
        "solution": solution_path,
    }


def rescore(out_dir: Path, size: str, row: dict) -> str:
    """Rescores with the shared evaluator and compares with the self-report."""
    if not row.get("solution") or not Path(row["solution"]).exists():
        return "n/a"
    instance = load_json(out_dir / f"instance_{size}.json")
    solution = load_json(row["solution"])
    rescored = format_score(score_assignment(instance, assignment_from_solution(instance, solution)))
    if rescored != row["score"]:
        rescored += " (self-report mismatch!)"
    return rescored


def track1(args) -> None:
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    sizes = [s.strip().upper() for s in args.sizes.split(",")]
    cpp_bin = ensure_cpp_bin(args.cpp_bin)

    print("\n== cross-check gate ==")
    crosscheck_gate(cpp_bin, out_dir)

    rows: dict[tuple[str, str], dict] = {}
    for size in sizes:
        print(f"\n== C++ engine: instance_{size} ({args.seconds}s) ==")
        rows[size, "cpp"] = run_cpp(cpp_bin, size, args.seed, args.seconds, out_dir)
        print(f"== CP-SAT: instance_{size} ({args.seconds}s, workers={args.workers}) ==")
        rows[size, "cpsat"] = run_cpsat(size, args.seed, args.seconds, out_dir, args.workers)

    lines = [
        f"## Track 1: quickstart six constraints ({args.seconds}s, seed {args.seed}, single seed — indicative only)",
        "",
        "| instance | solver | final (self-report) | final (rescored) | first feasible | moves/s |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for size in sizes:
        classes, teachers, rooms = PRESETS[size]
        label = f"{size}: {classes} classes/{teachers} teachers/{rooms} rooms"
        for solver in ("cpp", "cpsat"):
            row = rows.get((size, solver))
            if row is None:
                continue
            feasible_ms = row.get("first_feasible_ms")
            moves = row.get("moves_per_sec")
            lines.append(
                f"| {label} | {solver} | {row.get('score', 'n/a')} | {rescore(out_dir, size, row)} "
                f"| {f'{feasible_ms}ms' if feasible_ms is not None else 'n/a'} "
                f"| {f'{moves:,}' if moves else 'n/a'} |"
            )
    lines += [
        "",
        "- moves/s: C++ estimated from the improvement trajectory (one move per step). Not applicable to CP-SAT.",
        f"- CP-SAT runs {args.workers} parallel workers (portfolio); C++ runs single-threaded.",
        "- Rescoring: evaluate.py (the shared evaluator of the quickstart's six constraints).",
    ]
    report = "\n".join(lines)
    print("\n" + report)
    (out_dir / "RESULTS_track1.md").write_text(report + "\n", encoding="utf-8")


# ---------------------------------------------------------------- track 2


def _load_engine_solver():
    """The st_timetabling canonical entry point (`st_timetabling.solve`, default mode
    canonical). None when the import fails (extension not built, say)."""
    if str(ST_DIR / "python") not in sys.path:
        sys.path.insert(0, str(ST_DIR / "python"))
    try:
        import st_timetabling
    except ImportError:
        return None
    return st_timetabling.solve


def _load_engine_assembly():
    """`st_timetabling.constraints.Assembly` (None when not built / an older version)."""
    if str(ST_DIR / "python") not in sys.path:
        sys.path.insert(0, str(ST_DIR / "python"))
    try:
        from st_timetabling.constraints import Assembly
    except ImportError:
        return None
    return Assembly


def _drop_day(problem: dict, day: int) -> int:
    """Removes one weekday so the instance cannot be filled. Returns the cells lost per class.

    The periods stay and only cells go, so the shortfall is exact — no backend
    can place everything, and "how much was filled" becomes the comparison itself.

    Time references to that weekday (teacher/class unavailability, fixed and
    forbidden placements) are removed too — input validation rejects times not
    in the calendar.
    """
    per_day = problem["calendar"]["periods_per_day"]
    lost = int(per_day.get(str(day), 0))
    per_day[str(day)] = 0

    def prune(owner: dict, field: str) -> None:
        if field in owner:
            owner[field] = [slot for slot in owner[field] if slot["day"] != day]

    for teacher in problem["teachers"]:
        prune(teacher, "unavailable")
    for class_group in problem["class_groups"]:
        prune(class_group, "unavailable")
    for course in problem["courses"]:
        prune(course, "fixed_slots")
        prune(course, "forbidden_slots")
    return lost


def track3(args) -> None:
    """Track 3: partial timetabling — how much of an unfillable instance each backend fills.

    A track-2 instance loses one weekday so that periods > cells, and both sides
    solve with H1 (weekly periods) relaxed — CP-SAT with `solve_partial`, the
    engine with the equivalent assembly (`weekly_period_counts` hard=false). Both
    follow the same contract, "unfilled periods are a penalty, not a hard
    violation", so the comparison is **how much was filled** (placed periods) and
    the soft quality of the result.

    Both results are rescored with cpsat's validate.py, with H1 violations counted
    separately — here they are the intended shortfall, not a defect, and the
    other hard violations must be 0.
    """
    sys.path.insert(0, str(BENCH_DIR / "cpsat"))
    from pydantic import ValidationError
    from teacher_scheduling import SolverConfig, Timetable, TimetableProblem
    from teacher_scheduling.partial import UNASSIGNED_WEIGHT, solve_partial, unassigned_courses
    from teacher_scheduling.solver.config import ObjectiveWeights
    from teacher_scheduling.validate import compute_penalties, validate_timetable

    import gen_canonical

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    sizes = [s.strip().upper() for s in args.sizes.split(",")]
    weights = ObjectiveWeights()
    engine_solve = _load_engine_solver()
    Assembly = _load_engine_assembly()
    if engine_solve is None or Assembly is None:
        print("engine: pending (st_timetabling canonical not installed — running CP-SAT only)")

    def evaluate_result(problem, problem_dict, timetable) -> dict:
        """Shared rescoring. Separates H1 (the shortfall) from the other hard constraints.

        The other hard constraints are checked on **the placed part only** — the
        problem is re-validated with the weekly periods reduced to the placed counts,
        and courses with nothing placed are dropped. validate.py assumes a complete
        timetable and would report "no 2-period block" even for a block course
        dropped as a whole — in partial timetabling that is the intended shortfall,
        not a violation. Leaving only one cell of a 2-period block, on the other
        hand, is still caught as a violation here.
        """
        missing = sum(item.missing for item in unassigned_courses(problem, timetable))
        scheduled = Counter(lesson.course_id for lesson in timetable.lessons)
        shrunk = {
            **problem_dict,
            "courses": [
                course | {"periods_per_week": placed}
                for course in problem_dict["courses"]
                if (placed := scheduled.get(course["id"], 0))
            ],
        }
        try:
            others = validate_timetable(TimetableProblem.model_validate(shrunk), timetable)
        except ValidationError as exc:
            # The reduced problem itself does not hold = the placement shape is broken (e.g. a
            # 2-period block course with only one cell left). Surface it as a violation.
            others = [f"the partial assignment does not form a valid model: {exc}"]
        penalties = compute_penalties(problem, timetable)
        return {
            "placed": len(timetable.lessons),
            "missing": missing,
            "violations": others,
            "penalties": penalties,
            "weighted": sum(getattr(weights, k) * v for k, v in penalties.items()),
        }

    rows: list[dict] = []
    for size in sizes:
        problem_dict = gen_canonical.generate(size)
        required = sum(c["periods_per_week"] for c in problem_dict["courses"])
        lost = _drop_day(problem_dict, args.drop_day)
        with open(out_dir / f"partial_{size}.json", "w", encoding="utf-8") as f:
            json.dump(problem_dict, f, ensure_ascii=False, indent=2)
        problem = TimetableProblem.model_validate(problem_dict)
        capacity = sum(problem_dict["calendar"]["periods_per_day"].values()) * len(problem_dict["class_groups"])
        print(
            f"\n== partial_{size}: {required} periods, {capacity} cells (weekday {args.drop_day} removed: {lost} cells fewer per class) =="
        )

        print(f"== CP-SAT partial_{size} ({args.seconds}s) ==")
        timetable = solve_partial(
            problem,
            SolverConfig(
                time_limit_seconds=float(args.seconds),
                num_workers=args.workers,
                random_seed=args.seed,
                log_search_progress=False,
            ),
        )
        rows.append(
            {
                "size": size,
                "backend": "cpsat",
                "status": str(timetable.status),
                "wall": timetable.wall_time_seconds,
                "required": required,
                "capacity": capacity,
                **evaluate_result(problem, problem_dict, timetable),
            }
        )

        if engine_solve is None or Assembly is None:
            rows.append(
                {
                    "size": size,
                    "backend": "engine",
                    "status": "pending",
                    "wall": None,
                    "required": required,
                    "capacity": capacity,
                    "placed": None,
                    "missing": None,
                    "violations": [],
                    "penalties": {},
                    "weighted": None,
                }
            )
            continue

        print(f"== engine partial_{size} ({args.seconds}s) ==")
        t0 = time.perf_counter()
        try:
            # Same contract as cpsat's relaxed mode: one unassigned period = UNASSIGNED_WEIGHT.
            result = engine_solve(
                problem_dict,
                time_limit_seconds=float(args.seconds),
                random_seed=args.seed,
                constraints=Assembly().add("weekly_period_counts", hard=False, weight=UNASSIGNED_WEIGHT),
            )
        except Exception as e:  # noqa: BLE001 — show backend failures in the table as they are
            rows.append(
                {
                    "size": size,
                    "backend": "engine",
                    "status": f"error: {e}",
                    "wall": None,
                    "required": required,
                    "capacity": capacity,
                    "placed": None,
                    "missing": None,
                    "violations": [],
                    "penalties": {},
                    "weighted": None,
                }
            )
            continue
        wall = time.perf_counter() - t0
        timetable = Timetable.model_validate(result)
        rows.append(
            {
                "size": size,
                "backend": "engine",
                "status": str(timetable.status),
                "wall": wall,
                "required": required,
                "capacity": capacity,
                **evaluate_result(problem, problem_dict, timetable),
            }
        )

    lines = [
        f"## Track 3: partial timetabling — C++ canonical vs CP-SAT ({args.seconds}s, seed {args.seed})",
        "",
        f"Instances made unfillable (periods > cells) by removing weekday {args.drop_day}. Both sides relax H1"
        f" (CP-SAT `solve_partial`, engine `weekly_period_counts hard=false weight={UNASSIGNED_WEIGHT}`).",
        "",
        "| instance | backend | status | placed/required | unassigned | hard left | soft penalty | total objective | wall |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in rows:
        placed = "-" if row["placed"] is None else f"{row['placed']}/{row['required']}"
        missing = "-" if row["missing"] is None else str(row["missing"])
        hard = "-" if row["status"] == "pending" else str(len(row["violations"]))
        weighted = row["weighted"]
        wall = f"{row['wall']:.1f}s" if row["wall"] is not None else "-"
        # The value both solvers actually minimise: the unassigned penalty + the soft penalty.
        objective = "-" if weighted is None else str(row["missing"] * UNASSIGNED_WEIGHT + weighted)
        lines.append(
            f"| partial_{row['size']} | {row['backend']} | {row['status']} | {placed} "
            f"| {missing} | {hard} | {weighted if weighted is not None else '-'} | {objective} | {wall} |"
        )
    lines += [
        "",
        "- The **total objective** (unassigned×10000 + soft) is what both solvers actually minimise — lower is better.",
        "- **placed/required** dominates that objective: one more placed cell is worth 10000 soft points.",
        "- **hard left** are the hard violations besides unassigned lessons. Anything above 0 is a broken solution, not a partial one.",
        "- The weighted penalty is S1–S6 (default weights) over the placed part — it drops when less is placed, so read it with the placed count.",
        "- The two statuses use different axes: CP-SAT proves the relaxed model (OPTIMAL/FEASIBLE), the engine is FEASIBLE when the hard"
        " constraints besides unassigned lessons are 0.",
    ]
    for row in rows:
        if row["violations"]:
            lines += ["", f"### partial_{row['size']} / {row['backend']} remaining hard violations"]
            lines += [f"- {v}" for v in row["violations"][:20]]
    report = "\n".join(lines)
    print("\n" + report)
    (out_dir / "RESULTS_track3.md").write_text(report + "\n", encoding="utf-8")


def track2(args) -> None:
    sys.path.insert(0, str(BENCH_DIR / "cpsat"))
    from teacher_scheduling import SolverConfig, TimetableProblem, solve
    from teacher_scheduling.solver.config import ObjectiveWeights
    from teacher_scheduling.validate import compute_penalties, validate_timetable

    import gen_canonical

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    sizes = [s.strip().upper() for s in args.sizes.split(",")]
    weights = ObjectiveWeights()
    engine_solve = _load_engine_solver()
    if engine_solve is None:
        print("engine: pending (st_timetabling canonical not installed — running CP-SAT only)")

    def evaluate_result(problem, timetable) -> dict:
        violations = validate_timetable(problem, timetable)
        penalties = compute_penalties(problem, timetable)
        weighted = sum(getattr(weights, k) * v for k, v in penalties.items())
        return {"violations": violations, "penalties": penalties, "weighted": weighted}

    rows: list[dict] = []
    for size in sizes:
        problem_dict = gen_canonical.generate(size)
        path = out_dir / f"canonical_{size}.json"
        with open(path, "w", encoding="utf-8") as f:
            json.dump(problem_dict, f, ensure_ascii=False, indent=2)
        problem = TimetableProblem.model_validate(problem_dict)

        print(f"\n== CP-SAT canonical_{size} ({args.seconds}s) ==")
        timetable = solve(
            problem,
            SolverConfig(
                time_limit_seconds=float(args.seconds),
                num_workers=args.workers,
                random_seed=args.seed,
                log_search_progress=False,
            ),
        )
        checked = evaluate_result(problem, timetable)
        rows.append(
            {
                "size": size,
                "backend": "cpsat",
                "status": str(timetable.status),
                "wall": timetable.wall_time_seconds,
                **checked,
            }
        )

        if engine_solve is None:
            rows.append(
                {
                    "size": size,
                    "backend": "engine",
                    "status": "pending",
                    "wall": None,
                    "violations": [],
                    "penalties": {},
                    "weighted": None,
                }
            )
            continue
        print(f"== engine canonical_{size} ({args.seconds}s) ==")
        t0 = time.perf_counter()
        try:
            result = engine_solve(problem_dict, time_limit_seconds=float(args.seconds), random_seed=args.seed)
        except Exception as e:  # noqa: BLE001 — show backend failures in the table as they are
            rows.append(
                {
                    "size": size,
                    "backend": "engine",
                    "status": f"error: {e}",
                    "wall": None,
                    "violations": [],
                    "penalties": {},
                    "weighted": None,
                }
            )
            continue
        wall = time.perf_counter() - t0
        from teacher_scheduling import Timetable

        timetable = Timetable.model_validate(result)
        checked = evaluate_result(problem, timetable)
        rows.append({"size": size, "backend": "engine", "status": str(timetable.status), "wall": wall, **checked})

    lines = [
        f"## Track 2: full canonical constraints — C++ canonical vs CP-SAT ({args.seconds}s, seed {args.seed})",
        "",
        "| instance | backend | status | hard violations | weighted penalty | wall |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for row in rows:
        weighted = row["weighted"]
        wall = f"{row['wall']:.1f}s" if row["wall"] is not None else "-"
        lines.append(
            f"| canonical_{row['size']} | {row['backend']} | {row['status']} "
            f"| {len(row['violations']) if row['status'] not in ('pending',) else '-'} "
            f"| {weighted if weighted is not None else '-'} | {wall} |"
        )
    lines += [
        "",
        "- Both backends are rescored with cpsat validate.py (hard-violation list + S1–S6 penalties × default weights).",
        "- engine is the st_timetabling canonical backend (single thread; pending if the import fails).",
    ]
    for row in rows:
        if row["violations"]:
            lines += ["", f"### canonical_{row['size']} / {row['backend']} hard violation details"]
            lines += [f"- {v}" for v in row["violations"][:20]]
    report = "\n".join(lines)
    print("\n" + report)
    (out_dir / "RESULTS_track2.md").write_text(report + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--track2", action="store_true", help="run the full canonical-constraints track")
    parser.add_argument("--track3", action="store_true", help="run the partial-timetabling track (H1 relaxed)")
    parser.add_argument(
        "--drop-day",
        type=int,
        default=4,
        help="weekday removed in track 3 (0=Mon). Periods stay and only cells go, so it cannot be filled",
    )
    parser.add_argument("--sizes", default="S,M", help="comma-separated (track 1: the S,M,L presets)")
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--workers", type=int, default=8, help="CP-SAT worker count")
    parser.add_argument("--out-dir", default="out")
    parser.add_argument("--cpp-bin", help="path of st_quality_bench (default: built with cmake)")
    args = parser.parse_args()

    if args.track3:
        track3(args)
    elif args.track2:
        track2(args)
    else:
        track1(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
