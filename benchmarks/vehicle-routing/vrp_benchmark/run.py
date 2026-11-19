"""CVRP and VRPTW benchmark: optimaru vs OR-Tools Routing.

Every backend gets the same instance, fleet, integer distance matrix (and on
VRPTW the same time windows), wall-clock budget and seed, runs
single-threaded, and its routes go through the same independent `rescore()`.
The gap is measured against the CVRPLIB best-known solution. The fleet is the
`-kN` of the name on Augerat's Set A, which fixes the number of routes, a
bound that cannot bind on Set X, which leaves it open (`instance.free_fleet`),
and the file's VEHICLES on the VRPTW sets (Solomon, Gehring & Homberger),
scored by the DIMACS convention (`vrptw`).
"""

from __future__ import annotations

import argparse
import json
import statistics
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import asdict, dataclass
from functools import partial
from pathlib import Path
from typing import Callable, TypeVar

from . import solvers
from .cvrplib import (
    CATALOG_SOURCES,
    CatalogEntry,
    load_instance_text,
    load_x_catalog,
    resolve_mirror_bks,
)
from .instance import AUGERAT_A, CvrpInstance, load_augerat_instance, parse_instance, rescore
from .vrptw import CLASSES, VrptwEntry, homberger_entries, load_vrptw, solomon_entries

T = TypeVar("T")

DEFAULT_INSTANCE = "A-n32-k5"
SERIES_TITLES = {
    "x": "CVRPLIB Set X",
    "a": "CVRPLIB Set A (Augerat)",
    "solomon": "Solomon VRPTW",
    "homberger": "Gehring & Homberger VRPTW",
}
SIZE_BUCKETS = (
    ("100-200", 100, 200),
    ("200-400", 200, 400),
    ("400-600", 400, 600),
    ("600-800", 600, 800),
    ("800-1000", 800, 1_001),
)


@dataclass(frozen=True)
class BenchmarkResult:
    backend: str
    instance: str
    customer_count: int
    used_vehicles: int
    distance: int
    gap: float | None
    unassigned: int
    elapsed: float
    score: str
    seed: int = 0
    seconds: float = 0.0
    best_known_distance: int | None = None
    # False when a route exceeds the vehicle capacity or misses a time window.
    feasible: bool = True
    # `distance` and `best_known_distance` are in 1/scale units: 10 on VRPTW (tenths).
    scale: int = 1
    # VRPTW: C1 ... RC2; empty on CVRP.
    instance_class: str = ""


def parse_backends(spec: str) -> tuple[str, ...]:
    """`engine,ortools` / `all` -> ordered, de-duplicated backend names."""
    backends: list[str] = []
    for token in spec.split(","):
        token = token.strip()
        for backend in solvers.BACKENDS if token == "all" else (token,):
            if backend not in solvers.BACKENDS:
                raise ValueError(f"unknown backend {token!r}; expected {solvers.BACKENDS} or all")
            if backend not in backends:
                backends.append(backend)
    return tuple(backends)


def gap(distance: int, best_known_distance: int | None) -> float | None:
    """Gap to the BKS in percent, or None when there is no BKS to compare to."""
    if best_known_distance is None or best_known_distance <= 0:
        return None
    return 100 * (distance - best_known_distance) / best_known_distance


def benchmark(
    backend: str, instance: CvrpInstance, *, seconds: float, seed: int, instance_class: str = ""
) -> BenchmarkResult:
    solve = solvers.get(backend)
    started = time.perf_counter()
    outcome = solve(instance, seconds, seed)
    elapsed = time.perf_counter() - started
    checked = rescore(instance, outcome.routes)
    if outcome.reported_distance is not None and outcome.reported_distance != checked.distance:
        raise ValueError(f"{backend} reported {outcome.reported_distance}, the harness rescored {checked.distance}")
    complete = checked.feasible and checked.unassigned == 0
    return BenchmarkResult(
        backend=backend,
        instance=instance.name,
        customer_count=instance.customer_count,
        used_vehicles=checked.used_vehicles,
        distance=checked.distance,
        gap=gap(checked.distance, instance.best_known_distance) if complete else None,
        unassigned=checked.unassigned,
        elapsed=elapsed,
        score=outcome.summary,
        seed=seed,
        seconds=seconds,
        best_known_distance=instance.best_known_distance,
        feasible=checked.feasible,
        scale=instance.scale,
        instance_class=instance_class,
    )


def distance_text(value: int, scale: int) -> str:
    """A distance in the instance's unit: `27,591`, or `1,637.7` in tenths."""
    return f"{value:,}" if scale == 1 else f"{value / scale:,.1f}"


def status(result: BenchmarkResult) -> str:
    """Why a run has no gap: `n unassigned`, `infeasible` or `n/a` (no BKS)."""
    if result.unassigned > 0:
        return f"{result.unassigned} unassigned"
    if not result.feasible:
        return "infeasible"
    return "n/a"


def bucket(customer_count: int) -> str:
    for label, lower, upper in SIZE_BUCKETS:
        if lower <= customer_count < upper:
            return label
    raise ValueError(f"customer count outside the Set X buckets: {customer_count}")


# The groups a series' summary table averages over: size buckets on Set X and
# Gehring & Homberger, instance classes on Solomon (all of 100 customers).
HOMBERGER_GROUPS = tuple(str(size) for size in (200, 400, 600, 800, 1000))


def group_labels(series: str) -> tuple[str, ...]:
    if series == "x":
        return tuple(label for label, _, _ in SIZE_BUCKETS)
    if series == "homberger":
        return HOMBERGER_GROUPS
    if series == "solomon":
        return CLASSES
    return ()


def group_of(series: str) -> Callable[[BenchmarkResult], str]:
    if series == "x":
        return lambda result: bucket(result.customer_count)
    if series == "homberger":
        return lambda result: str(result.customer_count)
    if series == "solomon":
        return lambda result: result.instance_class
    raise ValueError(f"series {series!r} has no summary groups")


def even_sample(entries: list[T], limit: int) -> list[T]:
    if limit <= 0 or limit >= len(entries):
        return entries
    if limit == 1:
        return [entries[len(entries) // 2]]
    indices = {round(index * (len(entries) - 1) / (limit - 1)) for index in range(limit)}
    return [entries[index] for index in sorted(indices)]


def bucket_table(
    results: list[BenchmarkResult],
    backends: tuple[str, ...],
    catalog_sizes: dict[str, int],
    series: str = "x",
) -> list[dict]:
    """Per backend x group (size bucket or class): mean gap over complete runs, and coverage."""
    rows = []
    group = group_of(series)
    for backend in backends:
        for label in group_labels(series):
            in_bucket = [r for r in results if r.backend == backend and group(r) == label]
            solved = [r.gap for r in in_bucket if r.gap is not None]
            rows.append(
                {
                    "backend": backend,
                    "bucket": label,
                    "runs": len(in_bucket),
                    "solved": len(solved),
                    "catalog": catalog_sizes[label],
                    "mean_gap": statistics.fmean(solved) if solved else None,
                }
            )
    return rows


def markdown_summary(
    results: list[BenchmarkResult],
    rows: list[dict],
    backends: tuple[str, ...],
    args: argparse.Namespace,
) -> str:
    def cell(value: float | None) -> str:
        return "-" if value is None else f"{value:.2f}%"

    series = getattr(args, "series", None) or "x"
    if series == "x":
        selection = f"{args.limit_per_bucket or 'all'} instance(s) per bucket, catalog `{args.catalog_source}`"
    elif series == "homberger" and args.limit_per_bucket:
        selection = f"{args.limit_per_bucket} instance(s) per class and size"
    elif getattr(args, "only", None):
        selection = "the instances named with --only"
    else:
        selection = "every instance of the set"
    scoring = (
        " Distances are truncated to one decimal and the objective is distance alone (the DIMACS convention)."
        if series in ("solomon", "homberger")
        else ""
    )
    lines = [
        f"## {SERIES_TITLES[series]}: " + " vs ".join(backends),
        "",
        f"{args.seconds:g}s per run, seed {args.seed}, single-threaded, {selection}. "
        "Gap = (distance - BKS) / BKS over runs "
        "that served every customer within capacity (and time windows); every route re-verified by "
        f"the harness.{scoring}",
        "",
        "| Instance | Customers | " + " | ".join(backends) + " |",
        "|---|---:|" + "|".join("---:" for _ in backends) + "|",
    ]
    customers_of = {r.instance: r.customer_count for r in results}
    for name in sorted(customers_of, key=lambda n: (customers_of[n], n)):
        cells = []
        for backend in backends:
            match = [r for r in results if r.instance == name and r.backend == backend]
            if not match:
                cells.append("-")
            elif match[0].gap is None:
                cells.append(status(match[0]))
            else:
                cells.append(cell(match[0].gap))
        lines.append(f"| {name} | {customers_of[name]} | " + " | ".join(cells) + " |")
    if not rows:
        return "\n".join(lines) + "\n"
    lines += [
        "",
        f"| {'Class' if series == 'solomon' else 'Bucket'} | " + " | ".join(backends) + " |",
        "|---|" + "|".join("---:" for _ in backends) + "|",
    ]
    for label in group_labels(series):
        cells = []
        for backend in backends:
            row = next(r for r in rows if r["backend"] == backend and r["bucket"] == label)
            cells.append(f"{cell(row['mean_gap'])} ({row['solved']}/{row['runs']})")
        lines.append(f"| {label} | " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def write_outputs(
    args: argparse.Namespace,
    results: list[BenchmarkResult],
    backends: tuple[str, ...],
    catalog_sizes: dict[str, int] | None,
) -> None:
    """Write --out / --summary-markdown from what has finished so far. Called
    after every result, so a mid-series failure or a CI timeout still leaves
    the completed runs on disk. `catalog_sizes` is the size of each group of
    the series (Set X size buckets, Solomon classes, Gehring & Homberger
    sizes); None (Set A) leaves the group table out."""
    if args.out is None and args.summary_markdown is None:
        return
    series = getattr(args, "series", None) or "x"
    rows = [] if catalog_sizes is None else bucket_table(results, backends, catalog_sizes, series)
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        protocol = {
            "series": args.series,
            "seconds": args.seconds,
            "seed": args.seed,
            "backends": list(backends),
            # The instances `--only` named: a run of them is not a run of the series.
            "only": args.only,
            # Runs made side by side (`--jobs`): they share the machine's cores.
            "jobs": getattr(args, "jobs", 1),
        }
        if args.series in ("x", "homberger"):
            protocol["limit_per_bucket"] = args.limit_per_bucket
        if args.series == "x":
            protocol["catalog_source"] = args.catalog_source
        args.out.write_text(
            json.dumps(
                {"protocol": protocol, "results": [asdict(r) for r in results], "buckets": rows},
                indent=1,
            )
        )
    if args.summary_markdown is not None:
        args.summary_markdown.parent.mkdir(parents=True, exist_ok=True)
        args.summary_markdown.write_text(markdown_summary(results, rows, backends, args))


def print_result(result: BenchmarkResult) -> None:
    head = f"{result.instance:14} {result.backend:8}"
    if result.gap is None:
        print(f"{head} {status(result)} elapsed={result.elapsed:.2f}s score={result.score}", flush=True)
    else:
        print(
            f"{head} used={result.used_vehicles} distance={distance_text(result.distance, result.scale)} "
            f"gap={result.gap:.2f}% elapsed={result.elapsed:.2f}s",
            flush=True,
        )


def _run_one(load: Callable[[], CvrpInstance], backend: str, seconds: float, seed: int, instance_class: str):
    return benchmark(backend, load(), seconds=seconds, seed=seed, instance_class=instance_class)


def run_all(
    args: argparse.Namespace,
    jobs: list[tuple[str, Callable[[], CvrpInstance], str]],
    catalog_sizes: dict[str, int] | None,
) -> list[BenchmarkResult]:
    """Every (instance, backend) run of a series, `--jobs` of them at a time.

    `jobs` lists (name, loader, class) in the series' order. The loader is
    picklable: a worker process loads its own instance (from the cache the
    caller filled). Each run is single-threaded, so `--jobs` is the number of
    cores the series keeps busy; leave a core free, as a run that competes for
    one loses search time. Results stay in the series' order, then backend
    order, whatever order they finish in.
    """
    # Ordered by the run's own index, not by the name a parsed file carries.
    ranked: list[tuple[int, BenchmarkResult]] = []
    results: list[BenchmarkResult] = []

    def finished(index: int, result: BenchmarkResult) -> None:
        ranked.append((index, result))
        ranked.sort(key=lambda pair: pair[0])
        results[:] = [r for _, r in ranked]
        write_outputs(args, results, args.backend, catalog_sizes)
        print_result(result)

    runs = [(load, backend, cls) for _, load, cls in jobs for backend in args.backend]
    if args.jobs <= 1:
        for index, (load, backend, cls) in enumerate(runs):
            finished(index, _run_one(load, backend, args.seconds, args.seed, cls))
        return results
    pool = ProcessPoolExecutor(max_workers=args.jobs)
    try:
        futures = {
            pool.submit(_run_one, load, backend, args.seconds, args.seed, cls): index
            for index, (load, backend, cls) in enumerate(runs)
        }
        for future in as_completed(futures):
            finished(futures[future], future.result())
    except BaseException:
        # A failed run (or Ctrl-C) stops the series now: the queued runs are dropped, not run first.
        pool.shutdown(wait=False, cancel_futures=True)
        raise
    pool.shutdown()
    return results


def _load_x(entry: CatalogEntry, cache_dir: Path) -> CvrpInstance:
    return parse_instance(
        load_instance_text(entry, cache_dir),
        best_known_distance=entry.best_known_distance,
        proven_optimal=entry.proven_optimal,
        fixed_fleet=False,
    )


def _print_groups(title: str, results: list[BenchmarkResult], args, catalog_sizes: dict[str, int]) -> None:
    convention = " (DIMACS convention)" if args.series in ("solomon", "homberger") else ""
    print(f"\n{title} mean gap to the CVRPLIB BKS{convention} (complete runs / runs)")
    for row in bucket_table(results, args.backend, catalog_sizes, args.series):
        mean = "-" if row["mean_gap"] is None else f"{row['mean_gap']:.2f}%"
        print(f"{row['backend']:8} {row['bucket']:9} {mean:>8} ({row['solved']}/{row['runs']})")


def run_series(args: argparse.Namespace) -> int:
    catalog = load_x_catalog(args.cache_dir, refresh=args.refresh_catalog, source=args.catalog_source)
    by_bucket = {
        label: [entry for entry in catalog if bucket(entry.customer_count) == label] for label, _, _ in SIZE_BUCKETS
    }
    catalog_sizes = {label: len(entries) for label, entries in by_bucket.items()}
    selected = [entry for label, _, _ in SIZE_BUCKETS for entry in even_sample(by_bucket[label], args.limit_per_bucket)]
    if args.only:
        known = {entry.name: entry for entry in catalog}
        selected = [known[name] for name in _only(args.only, known)]
    if not selected:
        # A catalog page that parsed to nothing must not pass as a clean run.
        raise RuntimeError(f"catalog source {args.catalog_source!r} yielded no Set X instances")
    # Resolve every BKS and download every instance first: a download that
    # fails with nothing cached fails the run before any budget is spent.
    selected = [resolve_mirror_bks(entry, args.cache_dir, refresh=args.refresh_catalog) for entry in selected]
    for entry in selected:
        load_instance_text(entry, args.cache_dir)
    jobs = [(entry.name, partial(_load_x, entry, args.cache_dir), "") for entry in selected]
    results = run_all(args, jobs, catalog_sizes)
    _print_groups("Set X", results, args, catalog_sizes)
    return _incomplete_exit(args, results)


def run_set_a(args: argparse.Namespace) -> int:
    """Every Augerat Set A instance, smallest first; each file carries its optimum."""
    # Load everything first: a download that fails fails the run before any
    # budget is spent.
    names = _only(args.only, AUGERAT_A) if args.only else list(AUGERAT_A)
    for name in names:
        load_augerat_instance(name, args.cache_dir)
    jobs = [(name, partial(load_augerat_instance, name, args.cache_dir), "") for name in names]
    results = run_all(args, jobs, None)

    print("\nSet A mean gap to the optimum (complete runs / runs)")
    for backend in args.backend:
        runs = [r for r in results if r.backend == backend]
        solved = [r.gap for r in runs if r.gap is not None]
        mean = f"{statistics.fmean(solved):.2f}%" if solved else "-"
        print(f"{backend:8} {mean:>8} ({len(solved)}/{len(runs)})")
    return _incomplete_exit(args, results)


def vrptw_selection(series: str, limit: int) -> tuple[list[VrptwEntry], dict[str, int]]:
    """A VRPTW series' instances in run order, and the size of each summary group.

    `limit` (Gehring & Homberger only) samples that many instances evenly from
    each class at each size, so a short run still covers all 30 groups.
    """
    entries = list(solomon_entries() if series == "solomon" else homberger_entries())
    if series == "solomon":
        sizes = {cls: sum(e.instance_class == cls for e in entries) for cls in CLASSES}
        return entries, sizes
    sizes = {label: sum(str(e.customer_count) == label for e in entries) for label in HOMBERGER_GROUPS}
    if limit:
        cells: dict[tuple[int, str], list[VrptwEntry]] = {}
        for entry in entries:
            cells.setdefault((entry.customer_count, entry.instance_class), []).append(entry)
        entries = [entry for cell in cells.values() for entry in even_sample(cell, limit)]
    return entries, sizes


def run_vrptw(args: argparse.Namespace) -> int:
    """Solomon or Gehring & Homberger, scored by the DIMACS convention (see `vrptw`)."""
    entries, group_sizes = vrptw_selection(args.series, args.limit_per_bucket)
    if args.only:
        known = {entry.name: entry for entry in entries}
        entries = [known[name] for name in _only(args.only, known)]
    # Download everything first: a failed download fails the run before any budget is spent.
    for entry in entries:
        load_vrptw(entry, args.cache_dir)
    jobs = [(entry.name, partial(load_vrptw, entry, args.cache_dir), entry.instance_class) for entry in entries]
    results = run_all(args, jobs, group_sizes)
    _print_groups(SERIES_TITLES[args.series], results, args, group_sizes)
    return _incomplete_exit(args, results)


def _only(names: list[str], known) -> list[str]:
    """The `--only` names in the series' own order; an unknown name is an error."""
    unknown = sorted(set(names) - set(known))
    if unknown:
        raise SystemExit(f"--only: not in this series: {', '.join(unknown)}")
    return [name for name in known if name in names]


def _incomplete_exit(args: argparse.Namespace, results: list[BenchmarkResult]) -> int:
    incomplete = [r for r in results if r.backend == "engine" and r.gap is None]
    if args.fail_on_incomplete and incomplete:
        print("engine left an incomplete plan on: " + ", ".join(r.instance for r in incomplete))
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--instance", default=None, help=f"Augerat instance (default {DEFAULT_INSTANCE})")
    target.add_argument(
        "--series",
        choices=tuple(SERIES_TITLES),
        help=(
            "x: CVRPLIB Set X (100 instances); a: Augerat Set A (27 instances); "
            "solomon: Solomon VRPTW (56 instances, 100 customers); "
            "homberger: Gehring & Homberger VRPTW (300 instances, 200-1000 customers)"
        ),
    )
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument(
        "--backend",
        type=parse_backends,  # a bad name is a usage error, before any download
        default=solvers.BACKENDS,
        help=f"comma-separated subset of {','.join(solvers.BACKENDS)}, or all (default)",
    )
    parser.add_argument(
        "--limit-per-bucket",
        type=int,
        default=0,
        help=(
            "Set X: evenly sample N instances per size bucket; Gehring & Homberger: N per class and size; 0 runs all"
        ),
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=1,
        help="with --series: runs at a time, each single-threaded, in separate processes (default 1)",
    )
    parser.add_argument(
        "--only",
        action="append",
        default=None,
        metavar="NAME",
        help="with --series: run only this instance of the series; repeat for several",
    )
    parser.add_argument("--refresh-catalog", action="store_true")
    parser.add_argument(
        "--catalog-source",
        choices=CATALOG_SOURCES,
        default="auto",
        help="Set X catalog: official CVRPLIB page, the GitHub mirror, or auto",
    )
    parser.add_argument("--out", type=Path, default=None, help="write results JSON here")
    parser.add_argument("--summary-markdown", type=Path, default=None, help="write a Markdown summary here")
    parser.add_argument(
        "--fail-on-incomplete",
        action="store_true",
        help="exit 1 when the engine leaves a customer unassigned, a route over capacity or a window missed",
    )
    parser.add_argument("--cache-dir", type=Path, default=Path(".bench-cache"))
    args = parser.parse_args(argv)
    if args.fail_on_incomplete and "engine" not in args.backend:
        parser.error("--fail-on-incomplete inspects engine results; add engine to --backend")
    if args.only and not args.series:
        parser.error("--only selects instances of a --series; use --instance for a single Augerat instance")
    if args.limit_per_bucket and (args.series not in ("x", "homberger") or args.only):
        parser.error(
            "--limit-per-bucket samples Set X or Gehring & Homberger; it does not go with --only or another series"
        )
    if args.jobs < 1 or (args.jobs > 1 and not args.series):
        parser.error("--jobs takes a positive number of runs at a time, and goes with --series")

    if args.series == "x":
        return run_series(args)
    if args.series == "a":
        return run_set_a(args)
    if args.series in ("solomon", "homberger"):
        return run_vrptw(args)

    instance = load_augerat_instance(args.instance or DEFAULT_INSTANCE, args.cache_dir)
    print(
        f"{instance.name}: {instance.customer_count} customers, "
        f"{instance.vehicle_count} vehicles, capacity {instance.capacity}, "
        f"BKS {instance.best_known_distance}"
    )
    for backend in args.backend:
        print_result(benchmark(backend, instance, seconds=args.seconds, seed=args.seed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
