from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from bench.data import apply_permutation, load_suite
from optimaru import quadratic_assignment

DEFAULT_INSTANCES = ("nug30", "lipa30a", "tai30a")


def _objective(problem) -> int:
    costs = quadratic_assignment.constraint_costs(problem)
    return int(costs["flow_distance_cost"]["costs"][0])


def _gap(value: int, reference: int | None) -> float | None:
    if reference is None or reference == 0:
        return None
    return (value - reference) / abs(reference) * 100.0


#: Solution files whose stated value their own permutation does not reproduce, and what
#: the permutation scores. The kra32 file states 88900, the value of kra30a.
REFERENCE_ERRATA = {"kra32": 88700}


def reference_objective(name: str, source, reference) -> int:
    """What the QAPLIB reference permutation of instance `name` scores under the use
    case's evaluator.

    A solution file lists either the facility of each location or the location
    of each facility, and the archive does not say which, so both readings are
    scored. One of them must give the value the file states, or the value
    `REFERENCE_ERRATA` records for a file known to state another: anything else
    is a parsing or scoring bug and stops the run.
    """
    inverse = [0] * len(reference.permutation)
    for index, value in enumerate(reference.permutation):
        inverse[value] = index
    scores = []
    for permutation in (reference.permutation, tuple(inverse)):
        problem = source.model_copy(deep=True)
        apply_permutation(problem, permutation)
        scores.append(_objective(problem))
    if reference.objective in scores:
        return reference.objective
    if REFERENCE_ERRATA.get(name) in scores:
        return REFERENCE_ERRATA[name]
    raise AssertionError(
        f"QAPLIB reference mismatch for {name}: the solution file states {reference.objective}, "
        f"its permutation scores {' or '.join(map(str, scores))}"
    )


def _run_engine(problem, seconds: float, seed: int) -> dict:
    config = quadratic_assignment.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    started = time.perf_counter()
    problem = quadratic_assignment.solve(problem, config=config)
    elapsed = time.perf_counter() - started
    objective = _objective(problem)
    if objective != problem.total_cost:
        raise AssertionError(f"engine self-report mismatch: {problem.total_cost} != {objective}")
    return {
        "solver": "engine",
        "objective": objective,
        "seconds": elapsed,
        "feasible": problem.score_hard == 0,
    }


def _run_cpsat(problem, seconds: float, seed: int, workers: int) -> dict:
    from external_solver import quadratic_assignment as cpsat

    import quadratic_assignment_cpsat  # noqa: F401 - points external_solver at the checkout engine

    started = time.perf_counter()
    try:
        solved = cpsat.solve(
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
        raise AssertionError(f"CP-SAT self-report mismatch: {solved.total_cost} != {objective}")
    return {
        "solver": "cp-sat",
        "objective": objective,
        "seconds": elapsed,
        "feasible": solved.score_hard == 0,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run our QAP engines on QAPLIB instances with 30 to 50 facilities.")
    parser.add_argument(
        "--instance",
        action="append",
        help="QAPLIB stem; repeat to select multiple instances",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="run every instance of the benchmark's QAPLIB selection",
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
    parser.add_argument("--cache-dir", type=Path, default=Path(__file__).resolve().parent / ".cache")
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)

    suite = load_suite(args.cache_dir)
    selected = list(suite) if args.all else (args.instance or list(DEFAULT_INSTANCES))
    unknown = sorted(set(selected) - suite.keys())
    if unknown:
        parser.error(f"unknown instance(s): {', '.join(unknown)}; available: {', '.join(suite)}")

    results = []
    for name in selected:
        source, reference = suite[name]
        runs = [_run_engine(source.model_copy(deep=True), args.seconds, args.seed)]
        if args.exact_max_size and len(source.facilities) <= args.exact_max_size:
            runs.append(_run_cpsat(source.model_copy(deep=True), args.seconds, args.seed, args.workers))
        best_known = None if reference is None else reference_objective(name, source, reference)
        for run in runs:
            run["gap_to_qaplib_percent"] = None if run["objective"] is None else _gap(run["objective"], best_known)
        results.append(
            {
                "instance": name,
                "size": len(source.facilities),
                "qaplib_best_known": best_known,
                "budget": args.seconds,
                "seed": args.seed,
                "workers": args.workers,
                "exact_max_size": args.exact_max_size,
                "runs": runs,
            }
        )

    if args.as_json:
        print(json.dumps(results, indent=2))
        return 0
    print(f"QAPLIB — {args.seconds:g}s per solver, seed {args.seed}")
    print(f"{'instance':<12} {'n':>4} {'solver':<9} {'objective':>14} {'QAPLIB BKS':>14} {'gap':>9} {'seconds':>9}")
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
