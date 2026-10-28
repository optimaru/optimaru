"""school-timetabling 3-way 벤치마크 오케스트레이션.

트랙1 (quickstart 6제약, 기본): 같은 인스턴스를
  (1) 이 레포 C++ 엔진(`st_quality_bench`),
  (2) OR-Tools CP-SAT(`cpsat_quickstart.py`),
  (3) Java Timefold 원본(quickstarts 의 stock provider + benchmarks/school-timetabling/timefold 테스트)
로 풀어 마크다운 표(hard/soft, 최초 feasible 시각, moves/s)로 비교한다.
실행 전 크로스체크 게이트: C++ 와 Java 의 고정 인스턴스 스코어가
byte-identical 해야 벤치를 진행한다. 모든 해는 evaluate.py 로 재채점해서
self-report 와 대조한다.

트랙2 (--track2, canonical 전제약): gen_canonical.py 의 S/M/L canonical 문제를
CP-SAT(teacher_scheduling.solve)와 C++ 캐노니컬 엔진(st_timetabling.solve,
import 실패 시 "pending")으로 풀고, 두 결과 모두 cpsat validate.py 로
재채점한다 (같은 time limit, 같은 문제).

사용 예:
    uv run run_bench.py --sizes S,M --seconds 10 --quickstarts <clone 경로>
    uv run run_bench.py --track2 --sizes S,M,L --seconds 10

Java 트랙은 timefold-quickstarts 클론이 필요하다 (benchmarks/school-timetabling/timefold/README.md 의
mvnw 부트스트랩 참고). --quickstarts 를 생략하면 Java 는 건너뛴다.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

from evaluate import assignment_from_solution, format_score, load_json, score_assignment

BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parents[1]
ST_DIR = REPO_ROOT / "use-cases" / "school-timetabling"

# 트랙1 크기 프리셋: (학급, 교사, 교실). st_quality_bench.cpp 의 문서와 동일.
PRESETS: dict[str, tuple[int, int, int]] = {
    "S": (6, 12, 7),
    "M": (12, 20, 13),
    "L": (24, 40, 25),
}


def _run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    print(f"$ {' '.join(str(c) for c in cmd)}", flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, text=True, **kwargs)


def ensure_cpp_bin(explicit: str | None) -> Path:
    """st_quality_bench 바이너리를 찾거나 cmake 로 빌드한다."""
    if explicit:
        return Path(explicit)
    build_dir = REPO_ROOT / "build" / "st-bench"
    binary = build_dir / "st_quality_bench"
    _run(["cmake", "-S", ST_DIR / "cpp", "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release"], stdout=subprocess.DEVNULL)
    _run(["cmake", "--build", build_dir, "--target", "st_quality_bench", "-j"], stdout=subprocess.DEVNULL)
    return binary


# ---------------------------------------------------------------- Java 공통


def _mvnw(quickstarts: Path, extra: list[str], m2: str | None, timeout: float) -> str:
    module = quickstarts / "use-cases" / "school-timetabling"
    mvnw = module / "mvnw"
    if not mvnw.exists():
        raise SystemExit(f"{mvnw} 가 없다 — benchmarks/school-timetabling/timefold/README.md 의 mvnw 부트스트랩을 먼저 실행")
    # benchmarks/school-timetabling/timefold 의 테스트 2개를 quickstart 테스트 트리로 복사 (CI 와 동일).
    test_dir = module / "src/test/java/org/acme/schooltimetabling/solver"
    for java in (BENCH_DIR / "timefold").glob("*.java"):
        shutil.copy(java, test_dir / java.name)
    cmd = ["./mvnw", "-q", "-B", "test", "-Dsurefire.failIfNoSpecifiedTests=false", *extra]
    if m2:
        cmd.insert(1, f"-Dmaven.repo.local={m2}")
    print(f"$ (cd {module}) {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, cwd=module, text=True, capture_output=True, timeout=timeout, check=False)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout[-4000:] + proc.stderr[-2000:])
        raise SystemExit("mvnw test 실패")
    return proc.stdout


# ---------------------------------------------------------------- 크로스체크


def crosscheck_gate(cpp_bin: Path, quickstarts: Path | None, out_dir: Path, m2: str | None) -> None:
    """C++ 와 Java 스코어 계산기의 byte-identical 파리티 게이트."""
    crosscheck_json = out_dir / "crosscheck.json"
    proc = _run([cpp_bin, "crosscheck", crosscheck_json], capture_output=True)
    cpp_score = re.search(r"CROSSCHECK_SCORE=\S+", proc.stdout).group(0)
    print(f"C++ : {cpp_score}")

    # 덤프된 고정 할당을 evaluate.py 로도 재채점 (3중 체크).
    instance = load_json(crosscheck_json)
    eval_score = format_score(score_assignment(instance, [(l["timeslot"], l["room"]) for l in instance["lessons"]]))
    if cpp_score != f"CROSSCHECK_SCORE={eval_score}":
        raise SystemExit(f"evaluate.py 불일치: {eval_score}")
    print(f"eval: CROSSCHECK_SCORE={eval_score}")

    if quickstarts is None:
        print("Java: skipped (--quickstarts 미지정)")
        return
    stdout = _mvnw(
        quickstarts,
        ["-Dtest=CrossCheckScoreTest", f"-Dcrosscheck.file={crosscheck_json}"],
        m2,
        timeout=1800,
    )
    java_score = re.search(r"CROSSCHECK_SCORE=\S+", stdout).group(0)
    print(f"Java: {java_score}")
    if cpp_score != java_score:
        raise SystemExit(f"크로스체크 게이트 실패: C++ {cpp_score} != Java {java_score}")
    print("크로스체크 게이트: OK (byte-identical)")


# ---------------------------------------------------------------- 트랙1 러너


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
        "moves_per_sec": None,  # CP-SAT 는 move 개념이 없다
        "first_feasible_ms": meta["first_feasible_ms"],
        "solution": solution_path,
    }


def run_java(quickstarts: Path, sizes: list[str], seconds: int, out_dir: Path, m2: str | None) -> dict[str, dict]:
    stdout = _mvnw(
        quickstarts,
        ["-Dtest=QualityBenchTest", f"-Dqbench.dir={out_dir}", f"-Dqbench.seconds={seconds}"],
        m2,
        timeout=seconds * (len(sizes) + 1) + 1800,
    )
    results: dict[str, dict] = {}
    current: str | None = None
    for line in stdout.splitlines():
        started = re.match(r"QBENCH_JAVA instance=instance_(\w+)\.json", line)
        if started:
            current = started.group(1)
            results[current] = {"first_feasible_ms": None}
        feasible = re.match(r"QBENCH_JAVA first_feasible t=(\d+)ms", line)
        if feasible and current:
            results[current]["first_feasible_ms"] = int(feasible.group(1))
        final = re.match(
            r"QBENCH_JAVA_FINAL instance=instance_(\w+)\.json score=(\S+) "
            r"feasible=\S+ moves_per_sec=(\d+)",
            line,
        )
        if final:
            results[final.group(1)].update(
                score=final.group(2),
                moves_per_sec=int(final.group(3)),
                solution=out_dir / f"instance_{final.group(1)}.solution_java.json",
            )
    return results


def rescore(out_dir: Path, size: str, row: dict) -> str:
    """공유 평가기로 재채점해 self-report 와 대조한다."""
    if not row.get("solution") or not Path(row["solution"]).exists():
        return "n/a"
    instance = load_json(out_dir / f"instance_{size}.json")
    solution = load_json(row["solution"])
    rescored = format_score(score_assignment(instance, assignment_from_solution(instance, solution)))
    if rescored != row["score"]:
        rescored += " (self-report 불일치!)"
    return rescored


def track1(args) -> None:
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    sizes = [s.strip().upper() for s in args.sizes.split(",")]
    quickstarts = Path(args.quickstarts).resolve() if args.quickstarts else None
    cpp_bin = ensure_cpp_bin(args.cpp_bin)

    print("\n== 크로스체크 게이트 ==")
    crosscheck_gate(cpp_bin, quickstarts, out_dir, args.m2)

    rows: dict[tuple[str, str], dict] = {}
    for size in sizes:
        print(f"\n== C++ 엔진: instance_{size} ({args.seconds}s) ==")
        rows[size, "cpp"] = run_cpp(cpp_bin, size, args.seed, args.seconds, out_dir)
        print(f"== CP-SAT: instance_{size} ({args.seconds}s, workers={args.workers}) ==")
        rows[size, "cpsat"] = run_cpsat(size, args.seed, args.seconds, out_dir, args.workers)
    if quickstarts:
        print(f"\n== Java Timefold: {', '.join(sizes)} ({args.seconds}s) ==")
        for size, row in run_java(quickstarts, sizes, args.seconds, out_dir, args.m2).items():
            rows[size, "java"] = row

    lines = [
        f"## 트랙1: quickstart 6제약 3-way ({args.seconds}s, seed {args.seed}, 단일 시드 — 방향성 참고용)",
        "",
        "| instance | solver | final (self-report) | final (재채점) | 최초 feasible | moves/s |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for size in sizes:
        classes, teachers, rooms = PRESETS[size]
        label = f"{size}: {classes}학급/{teachers}교사/{rooms}교실"
        for solver in ("cpp", "cpsat", "java"):
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
        "- moves/s: C++ 는 개선 궤적 추정(스텝당 1 move), Java 는 솔버 자체 카운터. CP-SAT 는 해당 없음.",
        f"- CP-SAT 는 {args.workers} worker 병렬(포트폴리오), C++/Java 는 단일 스레드.",
        "- 재채점: evaluate.py (quickstart 6제약 공유 평가기).",
    ]
    report = "\n".join(lines)
    print("\n" + report)
    (out_dir / "RESULTS_track1.md").write_text(report + "\n", encoding="utf-8")


# ---------------------------------------------------------------- 트랙2


def _load_engine_solver():
    """st_timetabling canonical 진입점 (`st_timetabling.solve`, 기본 mode
    canonical). 확장 모듈 미빌드 등으로 import 가 실패하면 None."""
    if str(ST_DIR / "python") not in sys.path:
        sys.path.insert(0, str(ST_DIR / "python"))
    try:
        import st_timetabling
    except ImportError:
        return None
    return st_timetabling.solve


def _load_engine_assembly():
    """`st_timetabling.constraints.Assembly` (미빌드/구버전이면 None)."""
    if str(ST_DIR / "python") not in sys.path:
        sys.path.insert(0, str(ST_DIR / "python"))
    try:
        from st_timetabling.constraints import Assembly
    except ImportError:
        return None
    return Assembly


def _drop_day(problem: dict, day: int) -> int:
    """요일 하나를 지워 인스턴스를 못 채우게 만든다. 사라진 학급당 칸 수를 돌려준다.

    시수는 그대로 두고 칸만 줄이므로 부족분이 정확히 계산된다 — 어느 백엔드도
    전부 배정할 수 없고, "얼마나 채웠나" 가 그대로 비교 대상이 된다.

    그 요일을 가리키던 시간 참조(교사/학급 불가시간, 고정·금지 배치)도 같이
    지운다 — 캘린더에 없는 시간은 입력 검증이 거부한다.
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
    """트랙3: 부분 편성 — 못 채우는 인스턴스를 두 백엔드가 얼마나 채우는가.

    트랙2 인스턴스에서 요일 하나를 지워 시수 > 칸 으로 만든 뒤, 양쪽 다 H1(주당
    시수)을 완화해서 푼다 — CP-SAT 은 `solve_partial`, 엔진은 같은 뜻의 조립
    (`weekly_period_counts` hard=false). 둘 다 "못 채운 시수는 하드 위반이 아니라
    벌점" 이라는 같은 계약이므로, 비교 대상은 **얼마나 채웠는가**(배정 시수)와
    그렇게 채운 결과의 소프트 품질이다.

    두 결과 모두 cpsat 의 validate.py 로 재채점하되 H1 위반은 따로 센다 — 여기서
    그건 결함이 아니라 의도한 부족분이고, 나머지 하드 위반이 0 이어야 한다.
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
        print("engine: pending (st_timetabling canonical 미탑재 — CP-SAT 만 실행)")

    def evaluate_result(problem, problem_dict, timetable) -> dict:
        """공통 재채점. H1(부족분)과 나머지 하드를 갈라서 본다.

        나머지 하드는 **배정한 부분만** 놓고 본다 — 주당 시수를 실제 배정된 수로
        줄인 문제로 다시 검증하고, 한 칸도 못 넣은 강좌는 아예 뺀다. validate.py
        는 완성된 시간표를 전제로 쓰여서, 통째로 뺀 연강 강좌에도 "2연강이 없음"
        을 붙이기 때문이다 — 부분 편성에서 그건 위반이 아니라 의도한 부족분이다.
        반대로 2연강 중 1칸만 남기는 건 여기서 그대로 위반으로 잡힌다.
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
            # 줄인 문제 자체가 성립하지 않는다 = 배정 형태가 깨졌다 (예: 2연강
            # 강좌에 1칸만 남김). 위반으로 그대로 드러낸다.
            others = [f"부분 배정이 모델로 성립하지 않는다: {exc}"]
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
            f"\n== partial_{size}: 시수 {required}, 칸 {capacity} (요일 {args.drop_day} 삭제로 학급당 {lost}칸 감소) =="
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
            # cpsat 의 완화 모드와 같은 계약: 미배정 1시간 = UNASSIGNED_WEIGHT.
            result = engine_solve(
                problem_dict,
                time_limit_seconds=float(args.seconds),
                random_seed=args.seed,
                constraints=Assembly().add("weekly_period_counts", hard=False, weight=UNASSIGNED_WEIGHT),
            )
        except Exception as e:  # noqa: BLE001 — 백엔드 실패를 표에 그대로 드러낸다
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
        f"## 트랙3: 부분 편성 — C++ 캐노니컬 vs CP-SAT ({args.seconds}s, seed {args.seed})",
        "",
        f"요일 {args.drop_day} 를 지워 시수 > 칸 으로 만든 인스턴스. 양쪽 다 H1 완화"
        f" (CP-SAT `solve_partial`, engine `weekly_period_counts hard=false weight={UNASSIGNED_WEIGHT}`).",
        "",
        "| instance | backend | status | 배정/시수 | 미배정 | 남은 hard | 소프트 벌점 | 총 목적값 | wall |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in rows:
        placed = "-" if row["placed"] is None else f"{row['placed']}/{row['required']}"
        missing = "-" if row["missing"] is None else str(row["missing"])
        hard = "-" if row["status"] == "pending" else str(len(row["violations"]))
        weighted = row["weighted"]
        wall = f"{row['wall']:.1f}s" if row["wall"] is not None else "-"
        # 두 솔버가 실제로 최소화하는 값: 미배정 벌점 + 소프트 벌점.
        objective = "-" if weighted is None else str(row["missing"] * UNASSIGNED_WEIGHT + weighted)
        lines.append(
            f"| partial_{row['size']} | {row['backend']} | {row['status']} | {placed} "
            f"| {missing} | {hard} | {weighted if weighted is not None else '-'} | {objective} | {wall} |"
        )
    lines += [
        "",
        "- **총 목적값**(미배정×10000 + 소프트)이 두 솔버가 실제로 최소화하는 값이다 — 낮은 쪽이 낫다.",
        "- **배정/시수** 는 그 목적값의 지배항이다. 한 칸 더 채우는 것이 소프트 10000점만큼 값나간다.",
        "- **남은 hard** 는 미배정을 뺀 하드 위반이다. 0 이 아니면 부분해가 아니라 그냥 깨진 해다.",
        "- 가중 벌점은 배정한 부분에 대한 S1~S6 (기본 가중치) — 덜 채우면 낮아지므로 배정 수와 같이 본다.",
        "- 두 status 는 축이 다르다: CP-SAT 은 완화 모델을 증명해 OPTIMAL/FEASIBLE, engine 은 미배정을"
        " 뺀 하드가 0 이면 FEASIBLE 이다.",
    ]
    for row in rows:
        if row["violations"]:
            lines += ["", f"### partial_{row['size']} / {row['backend']} 남은 hard 위반"]
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
        print("engine: pending (st_timetabling canonical 미탑재 — CP-SAT 만 실행)")

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
        except Exception as e:  # noqa: BLE001 — 백엔드 실패를 표에 그대로 드러낸다
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
        f"## 트랙2: canonical 전제약 — C++ 캐노니컬 vs CP-SAT ({args.seconds}s, seed {args.seed})",
        "",
        "| instance | backend | status | hard 위반 | 가중 벌점 | wall |",
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
        "- 두 백엔드 모두 cpsat validate.py 로 재채점 (hard 위반 목록 + S1~S6 벌점 × 기본 가중치).",
        "- engine 은 st_timetabling canonical 백엔드 (단일 스레드; import 실패 시 pending).",
    ]
    for row in rows:
        if row["violations"]:
            lines += ["", f"### canonical_{row['size']} / {row['backend']} hard 위반 상세"]
            lines += [f"- {v}" for v in row["violations"][:20]]
    report = "\n".join(lines)
    print("\n" + report)
    (out_dir / "RESULTS_track2.md").write_text(report + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--track2", action="store_true", help="canonical 전제약 트랙 실행")
    parser.add_argument("--track3", action="store_true", help="부분 편성(H1 완화) 트랙 실행")
    parser.add_argument(
        "--drop-day",
        type=int,
        default=4,
        help="트랙3에서 지울 요일 (0=월). 시수는 그대로 두고 칸만 줄여 못 채우게 만든다",
    )
    parser.add_argument("--sizes", default="S,M", help="쉼표 구분 (트랙1: S,M,L 프리셋)")
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--workers", type=int, default=8, help="CP-SAT worker 수")
    parser.add_argument("--out-dir", default="out")
    parser.add_argument("--cpp-bin", help="st_quality_bench 경로 (기본: cmake 로 빌드)")
    parser.add_argument("--quickstarts", help="timefold-quickstarts 클론 경로 (Java 트랙)")
    parser.add_argument("--m2", help="-Dmaven.repo.local 로 쓸 Maven 캐시 경로")
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
