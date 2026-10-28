from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import quadratic_assignment

from bench.data import apply_permutation, load_suite

DEFAULT_INSTANCES = ("nug30", "lipa30a", "tai30a")


def _objective(problem) -> int:
    costs = quadratic_assignment.constraint_costs(problem)
    return int(costs["minimize_flow_distance"][0])


def _gap(value: int, reference: int | None) -> float | None:
    if reference is None or reference == 0:
        return None
    return (value - reference) / abs(reference) * 100.0


def _run_engine(problem, seconds: float, seed: int) -> dict:
    config = quadratic_assignment.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    config.max_iterations = 2_147_483_647
    started = time.perf_counter()
    quadratic_assignment.solve(problem, config=config)
    elapsed = time.perf_counter() - started
    objective = _objective(problem)
    if objective != problem.total_cost:
        raise AssertionError(
            f"engine self-report mismatch: {problem.total_cost} != {objective}"
        )
    return {
        "solver": "engine",
        "objective": objective,
        "seconds": elapsed,
        "feasible": problem.score_hard == 0,
    }


def _run_cpsat(problem, seconds: float, seed: int, workers: int) -> dict:
    import quadratic_assignment_cpsat

    started = time.perf_counter()
    try:
        solved = quadratic_assignment_cpsat.solve(
            problem,
            time_limit_seconds=seconds,
            random_seed=seed,
            num_workers=workers,
        )
    except RuntimeError as error:
        return {
            "solver": "cp-sat",
            "objective": None,
            "seconds": time.perf_counter() - started,
            "feasible": False,
            "status": str(error),
        }
    elapsed = time.perf_counter() - started
    objective = _objective(solved)
    if objective != solved.total_cost:
        raise AssertionError(
            f"CP-SAT self-report mismatch: {solved.total_cost} != {objective}"
        )
    return {
        "solver": "cp-sat",
        "objective": objective,
        "seconds": elapsed,
        "feasible": solved.score_hard == 0,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run our QAP engines on the public instances shipped by Hexaly."
    )
    parser.add_argument(
        "--instance",
        action="append",
        help="QAPLIB stem; repeat to select multiple instances",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="run every instance in Hexaly's QAP template archive",
    )
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument(
        "--exact-max-size",
        type=int,
        default=0,
        help="run the O(n^4) CP-SAT reference only up to this size; 0 disables it",
    )
    parser.add_argument(
        "--cache-dir", type=Path, default=Path(__file__).resolve().parent / ".cache"
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)

    suite = load_suite(args.cache_dir)
    selected = list(suite) if args.all else (args.instance or list(DEFAULT_INSTANCES))
    unknown = sorted(set(selected) - suite.keys())
    if unknown:
        parser.error(
            f"unknown instance(s): {', '.join(unknown)}; available: {', '.join(suite)}"
        )

    results = []
    for name in selected:
        source, reference = suite[name]
        runs = [_run_engine(source.model_copy(deep=True), args.seconds, args.seed)]
        if args.exact_max_size and len(source.facilities) <= args.exact_max_size:
            runs.append(
                _run_cpsat(
                    source.model_copy(deep=True), args.seconds, args.seed, args.workers
                )
            )
        reference_objective = reference.objective if reference else None
        if reference is not None:
            reference_problem = source.model_copy(deep=True)
            apply_permutation(reference_problem, reference.permutation)
            rescored = _objective(reference_problem)
            if rescored != reference.objective:
                raise AssertionError(
                    f"QAPLIB reference mismatch for {name}: {rescored} != {reference.objective}"
                )
        for run in runs:
            run["gap_to_qaplib_percent"] = (
                None
                if run["objective"] is None
                else _gap(run["objective"], reference_objective)
            )
        results.append(
            {
                "instance": name,
                "size": len(source.facilities),
                "qaplib_best_known": reference_objective,
                "runs": runs,
            }
        )

    if args.as_json:
        print(json.dumps(results, indent=2))
        return 0
    print(
        f"QAPLIB via Hexaly template — {args.seconds:g}s per solver, seed {args.seed}"
    )
    print(
        "Hexaly's published 60s aggregate gap is 1.1%; it does not publish per-instance assignments here."
    )
    print(
        f"{'instance':<12} {'n':>4} {'solver':<9} {'objective':>14} {'QAPLIB BKS':>14} {'gap':>9} {'seconds':>9}"
    )
    for result in results:
        for run in result["runs"]:
            gap = run["gap_to_qaplib_percent"]
            gap_text = "—" if gap is None else f"{gap:.2f}%"
            objective = run["objective"] if run["objective"] is not None else "—"
            print(
                f"{result['instance']:<12} {result['size']:>4} {run['solver']:<9} {objective!s:>14} "
                f"{result['qaplib_best_known'] or '—'!s:>14} {gap_text:>9} {run['seconds']:>9.3f}"
            )
            if run.get("status"):
                print(f"  {run['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
