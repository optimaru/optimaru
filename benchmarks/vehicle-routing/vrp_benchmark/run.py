"""CVRP benchmark: optimaru vs OR-Tools Routing.

Every backend gets the same instance, fleet size (`-kN`), rounded integer
distance matrix, wall-clock budget and seed, runs single-threaded, and its
routes go through the same independent `rescore()`. The gap is measured
against the CVRPLIB best-known solution.
"""

from __future__ import annotations

import argparse
import json
import statistics
import time
from dataclasses import asdict, dataclass
from pathlib import Path

from . import solvers
from .cvrplib import (
    CATALOG_SOURCES,
    CatalogEntry,
    load_instance_text,
    load_x_catalog,
    resolve_mirror_bks,
)
from .instance import CvrpInstance, load_augerat_instance, parse_instance, rescore

DEFAULT_INSTANCE = "A-n32-k5"
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
    # False when a route exceeds the vehicle capacity.
    feasible: bool = True


def parse_backends(spec: str) -> tuple[str, ...]:
    """`engine,ortools` / `all` -> ordered, de-duplicated backend names."""
    backends: list[str] = []
    for token in spec.split(","):
        token = token.strip()
        for backend in solvers.BACKENDS if token == "all" else (token,):
            if backend not in solvers.BACKENDS:
                raise ValueError(
                    f"unknown backend {token!r}; expected {solvers.BACKENDS} or all"
                )
            if backend not in backends:
                backends.append(backend)
    return tuple(backends)


def gap(distance: int, best_known_distance: int | None) -> float | None:
    """Gap to the BKS in percent, or None when there is no BKS to compare to."""
    if best_known_distance is None or best_known_distance <= 0:
        return None
    return 100 * (distance - best_known_distance) / best_known_distance


def benchmark(
    backend: str, instance: CvrpInstance, *, seconds: float, seed: int
) -> BenchmarkResult:
    solve = solvers.get(backend)
    started = time.perf_counter()
    outcome = solve(instance, seconds, seed)
    elapsed = time.perf_counter() - started
    checked = rescore(instance, outcome.routes)
    if outcome.reported_distance is not None and outcome.reported_distance != checked.distance:
        raise ValueError(
            f"{backend} reported {outcome.reported_distance}, "
            f"the harness rescored {checked.distance}"
        )
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
    )


def status(result: BenchmarkResult) -> str:
    """Why a run has no gap: `infeasible`, `n unassigned` or `n/a` (no BKS)."""
    if not result.feasible:
        return "infeasible"
    if result.unassigned > 0:
        return f"{result.unassigned} unassigned"
    return "n/a"


def bucket(customer_count: int) -> str:
    for label, lower, upper in SIZE_BUCKETS:
        if lower <= customer_count < upper:
            return label
    raise ValueError(f"customer count outside the Set X buckets: {customer_count}")


def even_sample(entries: list[CatalogEntry], limit: int) -> list[CatalogEntry]:
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
) -> list[dict]:
    """Per backend x bucket: mean gap over complete runs, and coverage."""
    rows = []
    for backend in backends:
        for label, _, _ in SIZE_BUCKETS:
            in_bucket = [
                r for r in results if r.backend == backend and bucket(r.customer_count) == label
            ]
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

    lines = [
        "## CVRPLIB Set X: " + " vs ".join(backends),
        "",
        f"{args.seconds:g}s per run, seed {args.seed}, single-threaded, "
        f"{args.limit_per_bucket or 'all'} instance(s) per bucket, "
        f"catalog `{args.catalog_source}`. Gap = (distance - BKS) / BKS over runs "
        "that served every customer within capacity; every route re-verified by "
        "the harness.",
        "",
        "| Instance | Customers | " + " | ".join(backends) + " |",
        "|---|---:|" + "|".join("---:" for _ in backends) + "|",
    ]
    for name in sorted({r.instance for r in results}, key=lambda n: int(n.split("-")[1][1:])):
        cells = []
        for backend in backends:
            match = [r for r in results if r.instance == name and r.backend == backend]
            if not match:
                cells.append("-")
            elif match[0].gap is None:
                cells.append(status(match[0]))
            else:
                cells.append(cell(match[0].gap))
        customers = next(r.customer_count for r in results if r.instance == name)
        lines.append(f"| {name} | {customers} | " + " | ".join(cells) + " |")
    lines += [
        "",
        "| Bucket | " + " | ".join(backends) + " |",
        "|---|" + "|".join("---:" for _ in backends) + "|",
    ]
    for label, _, _ in SIZE_BUCKETS:
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
    catalog_sizes: dict[str, int],
) -> None:
    """Write --out / --summary-markdown from what has finished so far. Called
    after every result, so a mid-series failure or a CI timeout still leaves
    the completed runs on disk."""
    if args.out is None and args.summary_markdown is None:
        return
    rows = bucket_table(results, backends, catalog_sizes)
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        protocol = {
            "series": "x",
            "seconds": args.seconds,
            "seed": args.seed,
            "limit_per_bucket": args.limit_per_bucket,
            "catalog_source": args.catalog_source,
            "backends": list(backends),
        }
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
        print(f"{head} {status(result)} elapsed={result.elapsed:.2f}s score={result.score}")
    else:
        print(
            f"{head} used={result.used_vehicles} distance={result.distance} "
            f"gap={result.gap:.2f}% elapsed={result.elapsed:.2f}s"
        )


def run_series(args: argparse.Namespace) -> int:
    catalog = load_x_catalog(args.cache_dir, refresh=args.refresh_catalog, source=args.catalog_source)
    by_bucket = {
        label: [entry for entry in catalog if bucket(entry.customer_count) == label]
        for label, _, _ in SIZE_BUCKETS
    }
    catalog_sizes = {label: len(entries) for label, entries in by_bucket.items()}
    selected = [
        entry
        for label, _, _ in SIZE_BUCKETS
        for entry in even_sample(by_bucket[label], args.limit_per_bucket)
    ]
    if not selected:
        # A catalog page that parsed to nothing must not pass as a clean run.
        raise RuntimeError(f"catalog source {args.catalog_source!r} yielded no Set X instances")
    # Resolve every BKS first: a download that fails with nothing cached
    # fails the run before any budget is spent.
    selected = [
        resolve_mirror_bks(entry, args.cache_dir, refresh=args.refresh_catalog)
        for entry in selected
    ]
    results: list[BenchmarkResult] = []
    for entry in selected:
        instance = parse_instance(
            load_instance_text(entry, args.cache_dir),
            best_known_distance=entry.best_known_distance,
            proven_optimal=entry.proven_optimal,
        )
        for backend in args.backend:
            result = benchmark(backend, instance, seconds=args.seconds, seed=args.seed)
            results.append(result)
            write_outputs(args, results, args.backend, catalog_sizes)
            print_result(result)

    print("\nSet X mean gap to the CVRPLIB BKS (complete runs / runs)")
    for row in bucket_table(results, args.backend, catalog_sizes):
        mean = "-" if row["mean_gap"] is None else f"{row['mean_gap']:.2f}%"
        print(f"{row['backend']:8} {row['bucket']:9} {mean:>8} ({row['solved']}/{row['runs']})")

    incomplete = [r for r in results if r.backend == "engine" and r.gap is None]
    if args.fail_on_incomplete and incomplete:
        print("engine left an incomplete plan on: " + ", ".join(r.instance for r in incomplete))
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--instance", default=None, help=f"Augerat instance (default {DEFAULT_INSTANCE})")
    target.add_argument("--series", choices=("x",), help="CVRPLIB Set X")
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
        help="Set X only: evenly sample N instances per size bucket; 0 runs all",
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
        help="exit 1 when the engine leaves a customer unassigned or a route over capacity",
    )
    parser.add_argument("--cache-dir", type=Path, default=Path(".bench-cache"))
    args = parser.parse_args(argv)
    if args.fail_on_incomplete and "engine" not in args.backend:
        parser.error("--fail-on-incomplete inspects engine results; add engine to --backend")

    if args.series:
        return run_series(args)

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
