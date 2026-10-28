from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import assembly_line_balancing

from bench.data import DATASETS, load_suite


def _gap(value: int, reference: int | None) -> float | None:
    if reference is None or reference == 0:
        return None
    return (value - reference) / reference * 100.0


def _rescore(problem) -> int:
    costs = assembly_line_balancing.constraint_costs(problem)
    soft = {"minimize_stations", "workload_balance"}
    hard = sum(
        sum(value) if isinstance(value, list) else value
        for name, value in costs.items()
        if name not in soft
    )
    if hard != 0:
        raise AssertionError(f"infeasible SALBP result: raw hard cost {hard}")
    return int(costs["minimize_stations"][0])


def _run_engine(problem, seconds: float, seed: int) -> dict:
    config = assembly_line_balancing.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    config.max_iterations = 2_147_483_647
    started = time.perf_counter()
    assembly_line_balancing.solve(problem, config=config)
    elapsed = time.perf_counter() - started
    objective = _rescore(problem)
    if objective != problem.stations_used:
        raise AssertionError(
            f"engine self-report mismatch: {problem.stations_used} != {objective}"
        )
    return {"solver": "engine", "stations": objective, "seconds": elapsed}


def _run_cpsat(problem, seconds: float, seed: int, workers: int) -> dict:
    import assembly_line_balancing_cpsat

    started = time.perf_counter()
    try:
        solved = assembly_line_balancing_cpsat.solve(
            problem,
            time_limit_seconds=seconds,
            random_seed=seed,
            num_workers=workers,
        )
    except RuntimeError as error:
        return {
            "solver": "cp-sat",
            "stations": None,
            "seconds": time.perf_counter() - started,
            "status": str(error),
        }
    elapsed = time.perf_counter() - started
    objective = _rescore(solved)
    if objective != solved.stations_used:
        raise AssertionError(
            f"CP-SAT self-report mismatch: {solved.stations_used} != {objective}"
        )
    return {"solver": "cp-sat", "stations": objective, "seconds": elapsed}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run our SALBP engines on the public dataset used by Hexaly."
    )
    parser.add_argument("--dataset", choices=DATASETS, default="large")
    parser.add_argument(
        "--instance",
        action="append",
        help="instance stem; repeat to select multiple instances",
    )
    parser.add_argument(
        "--all", action="store_true", help="run every instance in the selected dataset"
    )
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument(
        "--exact-max-size",
        type=int,
        default=100,
        help="run CP-SAT up to this task count; 0 disables it",
    )
    parser.add_argument(
        "--cache-dir", type=Path, default=Path(__file__).resolve().parent / ".cache"
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)

    suite = load_suite(args.cache_dir, args.dataset)
    selected = list(suite) if args.all else (args.instance or list(suite)[:3])
    unknown = sorted(set(selected) - suite.keys())
    if unknown:
        parser.error(f"unknown instance(s): {', '.join(unknown)}")

    results = []
    for name in selected:
        source, best_known = suite[name]
        runs = [_run_engine(source.model_copy(deep=True), args.seconds, args.seed)]
        if args.exact_max_size and len(source.tasks) <= args.exact_max_size:
            runs.append(
                _run_cpsat(
                    source.model_copy(deep=True), args.seconds, args.seed, args.workers
                )
            )
        for run in runs:
            run["gap_to_best_known_percent"] = (
                None if run["stations"] is None else _gap(run["stations"], best_known)
            )
        results.append(
            {
                "instance": name,
                "tasks": len(source.tasks),
                "best_known": best_known,
                "runs": runs,
            }
        )

    if args.as_json:
        print(json.dumps(results, indent=2))
        return 0
    print(f"SALBP {args.dataset} — {args.seconds:g}s per solver, seed {args.seed}")
    print(
        "Hexaly's published comparison uses the same large/very-large archive and a 60s budget."
    )
    print(
        f"{'instance':<28} {'n':>5} {'solver':<9} {'stations':>9} {'BKS':>7} {'gap':>9} {'seconds':>9}"
    )
    for result in results:
        for run in result["runs"]:
            gap = run["gap_to_best_known_percent"]
            gap_text = "—" if gap is None else f"{gap:.2f}%"
            stations = run["stations"] if run["stations"] is not None else "—"
            print(
                f"{result['instance']:<28} {result['tasks']:>5} {run['solver']:<9} {stations!s:>9} "
                f"{result['best_known'] or '—'!s:>7} {gap_text:>9} {run['seconds']:>9.3f}"
            )
            if run.get("status"):
                print(f"  {run['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
