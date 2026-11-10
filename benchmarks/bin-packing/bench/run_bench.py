from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path

from bench.data import load_suite
from optimaru import bin_packing

DEFAULT_INSTANCES = ("t60_00", "u120_04", "u250_03")


def sample_per_class(names: list[str], count: int) -> list[str]:
    """`count` instances of each class (the stem before `_`), evenly spread from its first to its last."""
    classes: dict[str, list[str]] = {}
    for name in names:
        classes.setdefault(name.rsplit("_", 1)[0], []).append(name)
    selected = []
    for members in classes.values():
        last = len(members) - 1
        if count <= 1 or last == 0:
            picks = {last // 2}
        else:
            picks = {(index * last + (count - 1) // 2) // (count - 1) for index in range(count)}
        selected += [members[pick] for pick in sorted(picks)]
    return selected


def _lower_bound(problem) -> int:
    return math.ceil(sum(item.weight for item in problem.items) / problem.bin_capacity)


def _run_engine(problem, seconds: float, seed: int) -> dict:
    core = problem.to_core()
    config = bin_packing.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = seed
    started = time.perf_counter()
    bin_packing.solve(core, config=config)
    elapsed = time.perf_counter() - started
    costs = bin_packing.constraint_costs(core)
    if sum(costs["bin_capacity_cost"]["costs"]) != 0:
        raise AssertionError("engine returned an overloaded bin")
    bins = int(costs["used_bins_cost"]["costs"][0])
    if bins != core.bins_used():
        raise AssertionError(f"engine self-report mismatch: {core.bins_used()} != {bins}")
    return {"solver": "engine", "bins": bins, "seconds": elapsed}


def _run_cpsat(problem, seconds: float, seed: int, workers: int) -> dict:
    from external_solver import bin_packing as cpsat

    import bin_packing_cpsat  # noqa: F401 - points external_solver at the checkout engine

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
            "bins": None,
            "seconds": time.perf_counter() - started,
            "status": str(error),
        }
    core = solved.to_core()
    costs = bin_packing.constraint_costs(core)
    if sum(costs["bin_capacity_cost"]["costs"]) != 0:
        raise AssertionError("CP-SAT returned an overloaded bin")
    bins = int(costs["used_bins_cost"]["costs"][0])
    if bins != solved.bins_used:
        raise AssertionError(f"CP-SAT self-report mismatch: {solved.bins_used} != {bins}")
    return {"solver": "cp-sat", "bins": bins, "seconds": time.perf_counter() - started}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare our solvers on Falkenauer's uniform and triplet instances (OR-Library)."
    )
    parser.add_argument(
        "--instance",
        action="append",
        help="instance stem; repeat to select multiple instances",
    )
    parser.add_argument("--all", action="store_true")
    parser.add_argument(
        "--sample-per-class",
        type=int,
        default=0,
        metavar="N",
        help="N instances of each of the eight classes, evenly spread (3: instances 00, 10 and 19)",
    )
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--exact-max-size", type=int, default=120)
    parser.add_argument("--cache-dir", type=Path, default=Path(__file__).resolve().parent / ".cache")
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)
    if sum(bool(choice) for choice in (args.all, args.sample_per_class, args.instance)) > 1:
        parser.error("--all, --sample-per-class and --instance are mutually exclusive")

    suite = load_suite(args.cache_dir)
    if args.all:
        selected = list(suite)
    elif args.sample_per_class:
        # The classes in the order of the OR-Library files: uniform, then triplets, smallest first.
        by_size = sorted(suite, key=lambda name: (name[0] != "u", len(suite[name].problem.items), name))
        selected = sample_per_class(by_size, args.sample_per_class)
    else:
        selected = args.instance or list(DEFAULT_INSTANCES)
    unknown = sorted(set(selected) - suite.keys())
    if unknown:
        parser.error(f"unknown instance(s): {', '.join(unknown)}")

    results = []
    for name in selected:
        source = suite[name].problem
        best_known = suite[name].best_known_bins
        lower_bound = _lower_bound(source)
        runs = [_run_engine(source.model_copy(deep=True), args.seconds, args.seed)]
        if args.exact_max_size and len(source.items) <= args.exact_max_size:
            runs.append(_run_cpsat(source.model_copy(deep=True), args.seconds, args.seed, args.workers))
        for run in runs:
            run["gap_to_capacity_lower_bound_percent"] = (
                None if run["bins"] is None else (run["bins"] - lower_bound) / lower_bound * 100.0
            )
            run["gap_to_best_known_percent"] = (
                None if run["bins"] is None else (run["bins"] - best_known) / best_known * 100.0
            )
        results.append(
            {
                "instance": name,
                "items": len(source.items),
                "lower_bound": lower_bound,
                "best_known": best_known,
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
    print(f"Falkenauer u/t instances (OR-Library) — {args.seconds:g}s per solver, seed {args.seed}")
    print(
        f"{'instance':<12} {'items':>6} {'solver':<9} {'bins':>7} {'weight LB':>10} {'LB gap':>9} {'best':>6} {'BKS gap':>9} {'seconds':>9}"
    )
    for result in results:
        for run in result["runs"]:
            gap = run["gap_to_capacity_lower_bound_percent"]
            gap_text = "—" if gap is None else f"{gap:.2f}%"
            bks_gap = run["gap_to_best_known_percent"]
            bks_gap_text = "—" if bks_gap is None else f"{bks_gap:.2f}%"
            bins = run["bins"] if run["bins"] is not None else "—"
            print(
                f"{result['instance']:<12} {result['items']:>6} {run['solver']:<9} {bins!s:>7} "
                f"{result['lower_bound']:>10} {gap_text:>9} "
                f"{result['best_known']:>6} {bks_gap_text:>9} {run['seconds']:>9.3f}"
            )
            if run.get("status"):
                print(f"  {run['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
