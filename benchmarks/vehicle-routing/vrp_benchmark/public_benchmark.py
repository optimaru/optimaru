"""Compare local CVRP backends with CVRPLIB BKS, PyVRP and Hexaly's published gaps.

The `pyvrp` backend runs PyVRP (hybrid genetic search) on the very same
instances, fleet size, integer distance matrix and wall-clock budget as the
local backends, and its routes go through the same independent `rescore()`.
That makes a `--backend engine,pyvrp` series the benchmark PyVRP publishes
for itself, reproduced locally with this engine beside it.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import itertools
import json
import math
import re
import statistics
import time
import urllib.request
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path

from vrp_routing.constraints import (
    maximize_visits_assigned,
    minimize_travel_time,
    service_finished_after_max_end_time,
    vehicle_capacity,
)
from vrp_routing.models import (
    RoutePlanRequest,
    RoutingInput,
    VehicleModel,
    VisitModel,
)
from vrp_routing.models import (
    solve_request as solve_engine_request,
)

from vehicle_routing_cpsat import solve_request as solve_cpsat_request
from .cvrplib import (
    CATALOG_SOURCES,
    CatalogEntry,
    load_instance_text,
    load_x_catalog,
    resolve_mirror_bks,
)

BACKENDS = ("engine", "cpsat", "pyvrp")
BACKEND_ALIASES = {"both": ("engine", "cpsat"), "all": BACKENDS}

ARCHIVE_URL = "https://www.hexaly.com/examples/cvrp.zip"
ARCHIVE_SHA256 = "97dec11d7a05d915b07e2fef3caaa21a1760cfecc5db6874b63cc835657a9511"
DEFAULT_INSTANCE = "A-n32-k5"
BENCHMARK_CONSTRAINTS = [
    vehicle_capacity,
    service_finished_after_max_end_time,
    minimize_travel_time,
    maximize_visits_assigned,
]
SIZE_BUCKETS = (
    ("100-200", 100, 200),
    ("200-400", 200, 400),
    ("400-600", 400, 600),
    ("600-800", 600, 800),
    ("800-1000", 800, 1_001),
)
# Published by Hexaly for Set X, Hexaly 15.0, 60 seconds, default parameters.
HEXALY_15_GAP_BY_BUCKET = {
    "100-200": 0.1,
    "200-400": 0.5,
    "400-600": 0.6,
    "600-800": 0.8,
    "800-1000": 0.9,
}
# Hexaly's article uses OR-Tools' Routing Model API, not this repository's
# independent CP-SAT circuit backend. Keep the two labels separate.
ORTOOLS_9_14_GAP_BY_BUCKET = {
    "100-200": 3.6,
    "200-400": 6.4,
    "400-600": 6.9,
    "600-800": 7.8,
    "800-1000": 5.4,
}


@dataclass(frozen=True)
class CvrpInstance:
    name: str
    vehicle_count: int
    capacity: int
    coordinates: tuple[tuple[int, int], ...]
    demands: tuple[int, ...]
    distances: tuple[tuple[int, ...], ...]
    best_known_distance: int | None
    proven_optimal: bool


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
    # False only when the backend's best plan violates a hard constraint
    # (PyVRP ran out of budget before a feasible plan appeared). An engine
    # run leaves customers unassigned instead, so it is always feasible.
    feasible: bool = True


@dataclass(frozen=True)
class _Stop:
    visit_id: str


def parse_backends(spec: str) -> tuple[str, ...]:
    """`engine,pyvrp` / `both` / `all` -> ordered backend names."""
    backends: list[str] = []
    for token in spec.split(","):
        token = token.strip()
        for backend in BACKEND_ALIASES.get(token, (token,)):
            if backend not in BACKENDS:
                raise ValueError(
                    f"unknown backend {token!r}; expected {BACKENDS} or both/all"
                )
            if backend not in backends:
                backends.append(backend)
    return tuple(backends)


def _rounded_euclidean(a: tuple[int, int], b: tuple[int, int]) -> int:
    return math.floor(math.dist(a, b) + 0.5)


def parse_instance(
    text: str,
    *,
    best_known_distance: int | None = None,
    proven_optimal: bool | None = None,
) -> CvrpInstance:
    """Parse a CVRPLIB EUC_2D instance with depot node 1."""
    headers: dict[str, str] = {}
    sections: dict[str, list[str]] = {}
    section: str | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line == "EOF":
            continue
        if line.endswith("_SECTION"):
            section = line
            sections[section] = []
        elif section is not None:
            sections[section].append(line)
        elif ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip()] = value.strip()

    if headers.get("EDGE_WEIGHT_TYPE") != "EUC_2D":
        raise ValueError("only EUC_2D CVRP instances are supported")
    dimension = int(headers["DIMENSION"])
    coordinates_by_id = {
        int(parts[0]): (int(parts[1]), int(parts[2]))
        for line in sections["NODE_COORD_SECTION"]
        if len(parts := line.split()) == 3
    }
    demands_by_id = {
        int(parts[0]): int(parts[1])
        for line in sections["DEMAND_SECTION"]
        if len(parts := line.split()) == 2
    }
    depot_tokens = [
        int(token) for line in sections["DEPOT_SECTION"] for token in line.split()
    ]
    if depot_tokens != [1, -1] or len(coordinates_by_id) != dimension:
        raise ValueError("expected one depot at node 1 and a complete coordinate set")
    coordinates = tuple(coordinates_by_id[i] for i in range(1, dimension + 1))
    demands = tuple(demands_by_id[i] for i in range(1, dimension + 1))
    if demands[0] != 0:
        raise ValueError("depot demand must be zero")
    distances = tuple(
        tuple(_rounded_euclidean(left, right) for right in coordinates)
        for left in coordinates
    )
    name = headers["NAME"]
    vehicle_match = re.search(r"-k(\d+)$", name)
    if vehicle_match is None:
        raise ValueError(f"instance name does not carry the fleet size: {name}")
    optimum_match = re.search(r"Optimal value:\s*(\d+)", headers.get("COMMENT", ""))
    comment_optimum = int(optimum_match.group(1)) if optimum_match else None
    return CvrpInstance(
        name=name,
        vehicle_count=int(vehicle_match.group(1)),
        capacity=int(headers["CAPACITY"]),
        coordinates=coordinates,
        demands=demands,
        distances=distances,
        best_known_distance=(
            best_known_distance if best_known_distance is not None else comment_optimum
        ),
        proven_optimal=(
            proven_optimal
            if proven_optimal is not None
            else comment_optimum is not None
        ),
    )


def _download(cache_dir: Path) -> bytes:
    cache_dir.mkdir(parents=True, exist_ok=True)
    archive = cache_dir / "cvrp.zip"
    if not archive.exists():
        with urllib.request.urlopen(ARCHIVE_URL, timeout=60) as response:
            archive.write_bytes(response.read())
    payload = archive.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if digest != ARCHIVE_SHA256:
        raise ValueError(f"cvrp.zip checksum mismatch: {digest}")
    return payload


def load_instance(name: str, cache_dir: Path) -> CvrpInstance:
    with zipfile.ZipFile(io.BytesIO(_download(cache_dir))) as archive:
        member = f"cvrp/instances/{name}.vrp"
        try:
            return parse_instance(archive.read(member).decode())
        except KeyError as exc:
            available = sorted(
                Path(item).stem
                for item in archive.namelist()
                if item.startswith("cvrp/instances/") and item.endswith(".vrp")
            )
            raise ValueError(
                f"unknown instance {name!r}; available: {', '.join(available)}"
            ) from exc


def _expanded_matrix(instance: CvrpInstance) -> list[list[int]]:
    # Python input orders one depot location per vehicle, then all customers.
    original = [0] * instance.vehicle_count + list(range(1, len(instance.demands)))
    return [[instance.distances[i][j] for j in original] for i in original]


def make_request(
    instance: CvrpInstance, *, seconds: float, seed: int, workers: int
) -> RoutePlanRequest:
    customer_count = len(instance.demands) - 1
    distance_upper_bound = max(map(max, instance.distances)) * (
        customer_count + instance.vehicle_count
    )
    depot = instance.coordinates[0]
    return RoutePlanRequest(
        name=instance.name,
        timelimit=seconds,
        random_seed=seed,
        num_workers=workers,
        model_input=RoutingInput(
            vehicles=[
                VehicleModel(
                    id=f"V{i + 1}",
                    capacity=instance.capacity,
                    depot=(float(depot[1]), float(depot[0])),
                    # CVRPLIB scores the round trip, so close every route.
                    return_to_depot=True,
                )
                for i in range(instance.vehicle_count)
            ],
            visits=[
                VisitModel(
                    id=str(i),
                    location=(float(y), float(x)),
                    demand=instance.demands[i],
                    max_end_time=distance_upper_bound + 1,
                    service_duration=60,
                    unassigned_penalty_minutes=1,
                )
                for i, (x, y) in enumerate(instance.coordinates[1:], start=1)
            ],
            driving_seconds=_expanded_matrix(instance),
        ),
    )


def rescore(instance: CvrpInstance, routes: dict[str, list]) -> tuple[int, int, int]:
    """Verify unique visits/capacity and return used, distance, unassigned."""
    seen: list[int] = []
    total_distance = 0
    used = 0
    for stops in routes.values():
        nodes = [int(stop.visit_id) for stop in stops]
        if not nodes:
            continue
        used += 1
        seen.extend(nodes)
        if sum(instance.demands[node] for node in nodes) > instance.capacity:
            raise ValueError("returned route exceeds vehicle capacity")
        total_distance += instance.distances[0][nodes[0]]
        total_distance += sum(
            instance.distances[left][right] for left, right in itertools.pairwise(nodes)
        )
        total_distance += instance.distances[nodes[-1]][0]
    if len(seen) != len(set(seen)):
        raise ValueError("returned routes visit a customer more than once")
    expected = set(range(1, len(instance.demands)))
    if not set(seen) <= expected:
        raise ValueError("returned routes contain an unknown customer")
    return used, total_distance, len(expected - set(seen))


def pyvrp_problem(instance: CvrpInstance):
    """Build PyVRP's ProblemData from the same rounded matrix the local
    backends use, with the instance's fleet size (`-kN`)."""
    import numpy as np
    from pyvrp import Client, Depot, Location, ProblemData, VehicleType

    locations = [Location(float(x), float(y)) for x, y in instance.coordinates]
    depots = [Depot(location=0)]
    clients = [
        Client(location=index, delivery=[instance.demands[index]])
        for index in range(1, len(instance.demands))
    ]
    vehicle_types = [
        VehicleType(num_available=instance.vehicle_count, capacity=[instance.capacity])
    ]
    matrix = np.asarray(instance.distances, dtype=np.int64)
    return ProblemData(
        locations,
        clients,
        depots,
        vehicle_types,
        distance_matrices=[matrix],
        # The engine request carries a 60s service duration per visit and a
        # deadline no route can reach (make_request); PyVRP gets zero
        # durations. Neither side's time dimension can bind, so both solvers
        # minimise the same rounded distance and the comparison is unaffected.
        duration_matrices=[np.zeros_like(matrix)],
    )


@dataclass(frozen=True)
class PyVrpOutcome:
    routes: dict[str, list]
    distance: int
    feasible: bool
    summary: str


def solve_pyvrp(instance: CvrpInstance, *, seconds: float, seed: int) -> PyVrpOutcome:
    """Run PyVRP on `instance` and return its routes, distance and feasibility."""
    try:
        from pyvrp import solve
        from pyvrp.stop import MaxRuntime
    except ImportError as exc:  # pragma: no cover - depends on the extra
        raise RuntimeError(
            "the pyvrp backend needs the `bench` extra: uv sync --extra bench"
        ) from exc
    result = solve(
        pyvrp_problem(instance), stop=MaxRuntime(seconds), seed=seed, display=False
    )
    # A client activity's idx counts clients from 0; the harness numbers
    # customers by CVRPLIB node, i.e. after the single depot.
    routes: dict[str, list[_Stop]] = {}
    for number, route in enumerate(result.best.routes(), start=1):
        routes[f"V{number}"] = [
            # PyVRP (0.14) numbers clients from 0 in ProblemData order, so
            # client idx maps to CVRPLIB customer idx + 1; rescore() rejects
            # duplicate or unknown customers if that convention ever changes.
            _Stop(visit_id=str(activity.idx + 1))
            for activity in route
            if activity.is_client()
        ]
    distance = int(result.best.distance())
    # Result.cost() is math.inf for an infeasible best, which int() rejects.
    feasible = bool(result.is_feasible())
    cost = f"cost={int(result.cost())}" if feasible else "cost=inf"
    return PyVrpOutcome(
        routes=routes,
        distance=distance,
        feasible=feasible,
        summary=f"pyvrp {'feasible' if feasible else 'infeasible'} {cost}",
    )


def _run(backend: str, request: RoutePlanRequest):
    started = time.perf_counter()
    if backend == "engine":
        # Monolithic fast path (repo bench convention): the four benchmark
        # constraints ARE the monolithic score semantics, and the plugin
        # assembly path only reaches 35-55% of its move throughput
        # (monolith-vs-plugin score parity is a standing vrp_plugin_test gate,
        # and rescore() below re-verifies every returned solution
        # independently). Measured on the 10s/6-seed Set X sample: mean gap
        # -2.5pp (X-n153) to -7.9pp (X-n701) versus forcing the plugin path.
        solution = solve_engine_request(request)
    else:
        solution = solve_cpsat_request(request, constraints=BENCHMARK_CONSTRAINTS)
    return solution, time.perf_counter() - started


def _gap(distance: int, best_known_distance: int | None) -> float | None:
    """Gap to the BKS in percent, or None when there is no BKS to compare to."""
    if best_known_distance is None or best_known_distance <= 0:
        return None
    return 100 * (distance - best_known_distance) / best_known_distance


def _benchmark(
    backend: str,
    instance: CvrpInstance,
    *,
    seconds: float,
    seed: int,
    workers: int,
) -> BenchmarkResult:
    feasible = True
    if backend == "pyvrp":
        started = time.perf_counter()
        outcome = solve_pyvrp(instance, seconds=seconds, seed=seed)
        routes, reported, score = outcome.routes, outcome.distance, outcome.summary
        elapsed = time.perf_counter() - started
        feasible = outcome.feasible
    else:
        request = make_request(instance, seconds=seconds, seed=seed, workers=workers)
        solution, elapsed = _run(backend, request)
        routes, reported, score = (
            solution.routes,
            solution.total_driving_seconds,
            str(solution.score),
        )
    common = dict(
        backend=backend,
        instance=instance.name,
        customer_count=len(instance.demands) - 1,
        elapsed=elapsed,
        score=score,
        seed=seed,
        seconds=seconds,
        best_known_distance=instance.best_known_distance,
    )
    if not feasible:
        # PyVRP keeps an over-capacity best when the budget ran out before a
        # feasible one appeared; report it as not solved, like an engine run
        # with unassigned customers, and do not hold its routes to the
        # rescore check (its own cost is infinite).
        return BenchmarkResult(
            used_vehicles=len([r for r in routes.values() if r]),
            distance=reported,
            gap=None,
            unassigned=0,
            feasible=False,
            **common,
        )
    used, distance, unassigned = rescore(instance, routes)
    if reported != distance:
        raise ValueError("solver report and independent distance disagree")
    return BenchmarkResult(
        used_vehicles=used,
        distance=distance,
        gap=(_gap(distance, instance.best_known_distance) if unassigned == 0 else None),
        unassigned=unassigned,
        **common,
    )


def _status(result: BenchmarkResult) -> str:
    """Why a run has no gap: `infeasible`, `n unassigned` or `n/a` (no BKS)."""
    if not result.feasible:
        return "infeasible"
    if result.unassigned > 0:
        return f"{result.unassigned} unassigned"
    return "n/a"  # fully assigned, no best-known distance to compare against


def _bucket(customer_count: int) -> str:
    for label, lower, upper in SIZE_BUCKETS:
        if lower <= customer_count < upper:
            return label
    raise ValueError(f"customer count outside Hexaly Set X buckets: {customer_count}")


def _even_sample(entries: list[CatalogEntry], limit: int) -> list[CatalogEntry]:
    if limit <= 0 or limit >= len(entries):
        return entries
    if limit == 1:
        return [entries[len(entries) // 2]]
    indices = {
        round(index * (len(entries) - 1) / (limit - 1)) for index in range(limit)
    }
    return [entries[index] for index in sorted(indices)]


def _bucket_table(
    results: list[BenchmarkResult],
    backends: tuple[str, ...],
    catalog_sizes: dict[str, int],
) -> list[dict]:
    """Per backend x bucket: mean gap over fully assigned runs, coverage."""
    rows = []
    for backend in backends:
        for label, _, _ in SIZE_BUCKETS:
            in_bucket = [
                result
                for result in results
                if result.backend == backend and _bucket(result.customer_count) == label
            ]
            solved = [result for result in in_bucket if result.gap is not None]
            rows.append(
                {
                    "backend": backend,
                    "bucket": label,
                    "runs": len(in_bucket),
                    "solved": len(solved),
                    "catalog": catalog_sizes[label],
                    "mean_gap": (
                        statistics.fmean(
                            result.gap for result in solved if result.gap is not None
                        )
                        if solved
                        else None
                    ),
                    "hexaly_15": HEXALY_15_GAP_BY_BUCKET[label],
                    "ortools_9_14": ORTOOLS_9_14_GAP_BY_BUCKET[label],
                }
            )
    return rows


def _markdown_summary(
    results: list[BenchmarkResult], rows: list[dict], backends: tuple[str, ...], args
) -> str:
    def gap(value: float | None) -> str:
        return "-" if value is None else f"{value:.2f}%"

    lines = [
        "## CVRPLIB Set X: " + " vs ".join(backends),
        "",
        f"{args.seconds:g}s per run, seed {args.seed}, {args.workers} worker(s), "
        f"{args.limit_per_bucket or 'all'} instance(s) per bucket, catalog `{args.catalog_source}`. "
        "Gap = (distance - BKS) / BKS over runs that assigned every customer; "
        "every route re-verified by the harness.",
        "",
        "| Instance | Customers | " + " | ".join(backends) + " |",
        "|---|---:|" + "|".join("---:" for _ in backends) + "|",
    ]
    for name in sorted(
        {r.instance for r in results}, key=lambda n: int(n.split("-")[1][1:])
    ):
        cells = []
        for backend in backends:
            match = [r for r in results if r.instance == name and r.backend == backend]
            if not match:
                cells.append("-")
            elif match[0].gap is None:
                cells.append(_status(match[0]))
            else:
                cells.append(gap(match[0].gap))
        customers = next(r.customer_count for r in results if r.instance == name)
        lines.append(f"| {name} | {customers} | " + " | ".join(cells) + " |")
    lines += [
        "",
        "| Bucket | " + " | ".join(backends) + " | Hexaly 15.0 (published) |",
        "|---|" + "|".join("---:" for _ in backends) + "|---:|",
    ]
    for label, _, _ in SIZE_BUCKETS:
        cells = []
        for backend in backends:
            row = next(
                r for r in rows if r["backend"] == backend and r["bucket"] == label
            )
            cells.append(f"{gap(row['mean_gap'])} ({row['solved']}/{row['runs']})")
        lines.append(
            f"| {label} | "
            + " | ".join(cells)
            + f" | {HEXALY_15_GAP_BY_BUCKET[label]:.1f}% |"
        )
    lines += [
        "",
        "Hexaly's column is its published full-Set-X average (60s, AMD Ryzen 7 7700); "
        "the local columns are this run on this machine.",
    ]
    return "\n".join(lines) + "\n"


def _write_outputs(
    args: argparse.Namespace,
    results: list[BenchmarkResult],
    backends: tuple[str, ...],
    all_by_bucket: dict[str, list[CatalogEntry]],
) -> None:
    """Write --out / --summary-markdown from what has finished so far. Called
    after every result, so a mid-series failure or a CI timeout still leaves
    the completed runs on disk."""
    if args.out is None and args.summary_markdown is None:
        return
    rows = _bucket_table(
        results,
        backends,
        {label: len(all_by_bucket[label]) for label, _, _ in SIZE_BUCKETS},
    )
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(
            json.dumps(
                {
                    "protocol": {
                        "series": "x",
                        "seconds": args.seconds,
                        "seed": args.seed,
                        "workers": args.workers,
                        "limit_per_bucket": args.limit_per_bucket,
                        "catalog_source": args.catalog_source,
                        "backends": list(backends),
                    },
                    "results": [asdict(result) for result in results],
                    "buckets": rows,
                },
                indent=1,
            )
        )
    if args.summary_markdown is not None:
        args.summary_markdown.parent.mkdir(parents=True, exist_ok=True)
        args.summary_markdown.write_text(
            _markdown_summary(results, rows, backends, args)
        )


def _run_series(args: argparse.Namespace) -> int:
    catalog = load_x_catalog(
        args.cache_dir, refresh=args.refresh_catalog, source=args.catalog_source
    )
    all_by_bucket = {
        label: [entry for entry in catalog if _bucket(entry.customer_count) == label]
        for label, _, _ in SIZE_BUCKETS
    }
    selected = [
        entry
        for label, _, _ in SIZE_BUCKETS
        for entry in _even_sample(all_by_bucket[label], args.limit_per_bucket)
    ]
    if not selected:
        # A catalog page that parsed to nothing must not pass as "nothing
        # was left unassigned".
        raise RuntimeError(
            f"catalog source {args.catalog_source!r} yielded no Set X instances"
        )
    backends = args.backend
    results: list[BenchmarkResult] = []
    # Resolve every BKS first: a download that fails with nothing cached
    # fails the run before any budget is spent.
    selected = [
        resolve_mirror_bks(entry, args.cache_dir, refresh=args.refresh_catalog)
        for entry in selected
    ]
    for entry in selected:
        instance = parse_instance(
            load_instance_text(entry, args.cache_dir),
            best_known_distance=entry.best_known_distance,
            proven_optimal=entry.proven_optimal,
        )
        for backend in backends:
            if backend == "cpsat" and entry.customer_count > args.cpsat_max_customers:
                print(
                    f"{entry.name:14} cpsat  skipped: {entry.customer_count} customers "
                    f"> --cpsat-max-customers {args.cpsat_max_customers}"
                )
                continue
            result = _benchmark(
                backend,
                instance,
                seconds=args.seconds,
                seed=args.seed,
                workers=args.workers,
            )
            results.append(result)
            _write_outputs(args, results, backends, all_by_bucket)
            if result.gap is None:
                print(
                    f"{result.instance:14} {backend:6} {_status(result)} "
                    f"elapsed={result.elapsed:.2f}s score={result.score}"
                )
            else:
                print(
                    f"{result.instance:14} {backend:6} used={result.used_vehicles} "
                    f"distance={result.distance} gap={result.gap:.2f}% "
                    f"elapsed={result.elapsed:.2f}s"
                )

    print("\nSet X average gap to CVRPLIB BKS")
    print(
        "backend bucket    solved/catalog local   Hexaly15  OR-Tools9.14 delta-v-Hexaly"
    )
    for backend in backends:
        for label, _, _ in SIZE_BUCKETS:
            bucket_results = [
                result
                for result in results
                if result.backend == backend
                and _bucket(result.customer_count) == label
                and result.gap is not None
            ]
            coverage = f"{len(bucket_results)}/{len(all_by_bucket[label])}"
            if bucket_results:
                local_gap = statistics.fmean(
                    result.gap for result in bucket_results if result.gap is not None
                )
                official_gap = HEXALY_15_GAP_BY_BUCKET[label]
                ortools_gap = ORTOOLS_9_14_GAP_BY_BUCKET[label]
                print(
                    f"{backend:7} {label:10} {coverage:14} "
                    f"{local_gap:6.2f}% {official_gap:8.2f}% "
                    f"{ortools_gap:13.2f}% {local_gap - official_gap:+9.2f}pp"
                )
            else:
                print(
                    f"{backend:7} {label:10} {coverage:14} {'-':>7} "
                    f"{HEXALY_15_GAP_BY_BUCKET[label]:8.2f}% "
                    f"{ORTOOLS_9_14_GAP_BY_BUCKET[label]:13.2f}% {'-':>13}"
                )
    print(
        "Hexaly reference: all Set X instances, 60 seconds, AMD Ryzen 7 7700; "
        "a partial local sample or another runtime is not an apples-to-apples reproduction."
    )

    _write_outputs(args, results, backends, all_by_bucket)

    incomplete = [r for r in results if r.backend == "engine" and r.unassigned]
    if args.fail_on_unassigned and incomplete:
        print(
            "engine left customers unassigned on: "
            + ", ".join(f"{r.instance} ({r.unassigned})" for r in incomplete)
        )
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--instance", default=None)
    target.add_argument("--series", choices=("x",))
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument(
        "--backend",
        type=parse_backends,  # a bad name is a usage error, before any download
        default=parse_backends("both"),
        help="comma-separated subset of engine,cpsat,pyvrp; `both` = engine,cpsat, `all`",
    )
    parser.add_argument(
        "--limit-per-bucket",
        type=int,
        default=0,
        help="Set X only: evenly sample N instances per Hexaly size bucket; 0 runs all",
    )
    parser.add_argument(
        "--cpsat-max-customers",
        type=int,
        default=200,
        help="skip larger CP-SAT circuit models during a Set X series run",
    )
    parser.add_argument("--refresh-catalog", action="store_true")
    parser.add_argument(
        "--catalog-source",
        choices=CATALOG_SOURCES,
        default="auto",
        help="Set X catalog: official CVRPLIB page, the PyVRP/Instances mirror, or auto",
    )
    parser.add_argument(
        "--out", type=Path, default=None, help="write results JSON here"
    )
    parser.add_argument(
        "--summary-markdown",
        type=Path,
        default=None,
        help="write a Markdown summary here",
    )
    parser.add_argument(
        "--fail-on-unassigned",
        action="store_true",
        help="exit 1 when the engine leaves a customer unassigned on any instance",
    )
    parser.add_argument(
        "--cache-dir", type=Path, default=Path(__file__).with_name(".cache")
    )
    args = parser.parse_args(argv)
    if args.fail_on_unassigned and "engine" not in args.backend:
        parser.error(
            "--fail-on-unassigned inspects engine results; add engine to --backend"
        )

    if args.series:
        return _run_series(args)

    instance = load_instance(args.instance or DEFAULT_INSTANCE, args.cache_dir)
    print(
        f"{instance.name}: {len(instance.demands) - 1} customers, "
        f"{instance.vehicle_count} vehicles, capacity {instance.capacity}, "
        f"BKS {instance.best_known_distance}"
    )
    for backend in args.backend:
        result = _benchmark(
            backend,
            instance,
            seconds=args.seconds,
            seed=args.seed,
            workers=args.workers,
        )
        if result.gap is None:
            print(
                f"{backend:6} {_status(result)} "
                f"elapsed={result.elapsed:.2f}s score={result.score}"
            )
        else:
            print(
                f"{backend:6} used={result.used_vehicles} distance={result.distance} "
                f"gap={result.gap:.2f}% elapsed={result.elapsed:.2f}s "
                f"score={result.score}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
