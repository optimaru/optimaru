from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path

import bin_packing

from bench.data import load_suite

DEFAULT_INSTANCES = ("t60_00", "u120_04", "u250_03")


def _lower_bound(problem) -> int:
    return math.ceil(sum(item.weight for item in problem.items) / problem.bin_capacity)


def _run_engine(problem, seconds: float, seed: int) -> dict:
    core = problem.to_core()
    config = bin_packing.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    config.max_iterations = 2_147_483_647
    started = time.perf_counter()
    bin_packing.solve(core, config=config)
    elapsed = time.perf_counter() - started
    costs = bin_packing.constraint_costs(core)
    if sum(costs["bin_capacity"]["costs"]) != 0:
        raise AssertionError("engine returned an overloaded bin")
    bins = int(costs["minimize_bins"]["costs"][0])
    if bins != core.bins_used():
        raise AssertionError(
            f"engine self-report mismatch: {core.bins_used()} != {bins}"
        )
    return {"solver": "engine", "bins": bins, "seconds": elapsed}


def _run_cpsat(problem, seconds: float, seed: int, workers: int) -> dict:
    import bin_packing_cpsat

    started = time.perf_counter()
    try:
        solved = bin_packing_cpsat.solve(
            problem,
            time_limit_seconds=seconds,
            random_seed=seed,
            num_workers=workers,
        )
    except RuntimeError as error:
        return {
            "solver": "cp-sat",
            "bins": None,
            "seconds": time.perf_counter() - started,
            "status": str(error),
        }
    core = solved.to_core()
    costs = bin_packing.constraint_costs(core)
    if sum(costs["bin_capacity"]["costs"]) != 0:
        raise AssertionError("CP-SAT returned an overloaded bin")
    bins = int(costs["minimize_bins"]["costs"][0])
    if bins != solved.bins_used:
        raise AssertionError(
            f"CP-SAT self-report mismatch: {solved.bins_used} != {bins}"
        )
    return {"solver": "cp-sat", "bins": bins, "seconds": time.perf_counter() - started}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare our solvers on the instances shipped by Hexaly's BPP template."
    )
    parser.add_argument(
        "--instance",
        action="append",
        help="instance stem; repeat to select multiple instances",
    )
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--exact-max-size", type=int, default=120)
    parser.add_argument(
        "--cache-dir", type=Path, default=Path(__file__).resolve().parent / ".cache"
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)

    suite = load_suite(args.cache_dir)
    selected = list(suite) if args.all else (args.instance or list(DEFAULT_INSTANCES))
    unknown = sorted(set(selected) - suite.keys())
    if unknown:
        parser.error(f"unknown instance(s): {', '.join(unknown)}")

    results = []
    for name in selected:
        source = suite[name]
        lower_bound = _lower_bound(source)
        runs = [_run_engine(source.model_copy(deep=True), args.seconds, args.seed)]
        if args.exact_max_size and len(source.items) <= args.exact_max_size:
            runs.append(
                _run_cpsat(
                    source.model_copy(deep=True), args.seconds, args.seed, args.workers
                )
            )
        for run in runs:
            run["gap_to_capacity_lower_bound_percent"] = (
                None
                if run["bins"] is None
                else (run["bins"] - lower_bound) / lower_bound * 100.0
            )
        results.append(
            {
                "instance": name,
                "items": len(source.items),
                "lower_bound": lower_bound,
                "runs": runs,
            }
        )

    if args.as_json:
        print(json.dumps(results, indent=2))
        return 0
    print(
        f"Hexaly BPP template instances — {args.seconds:g}s per solver, seed {args.seed}"
    )
    print(
        "These t/u files are not the BPPLIB GI collection used in Hexaly's 2024 benchmark article."
    )
    print(
        f"{'instance':<12} {'items':>6} {'solver':<9} {'bins':>7} {'weight LB':>10} {'LB gap':>9} {'seconds':>9}"
    )
    for result in results:
        for run in result["runs"]:
            gap = run["gap_to_capacity_lower_bound_percent"]
            gap_text = "—" if gap is None else f"{gap:.2f}%"
            bins = run["bins"] if run["bins"] is not None else "—"
            print(
                f"{result['instance']:<12} {result['items']:>6} {run['solver']:<9} {bins!s:>7} "
                f"{result['lower_bound']:>10} {gap_text:>9} {run['seconds']:>9.3f}"
            )
            if run.get("status"):
                print(f"  {run['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
