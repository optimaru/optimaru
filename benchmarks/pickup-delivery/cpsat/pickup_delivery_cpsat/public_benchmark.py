"""Compare both backends on Li & Lim PDPTW instances (SINTEF TOP archive or synthetic)."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import random
import sys
import time
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

from external_solver.pickup_delivery import solve_request as solve_cpsat_request

from cpsat_plugins.results import write_results, write_whole
from optimaru.pickup_delivery.constraints import (
    max_end_time_cost,
    paired_request_cost,
    pickup_before_dropoff_cost,
    same_vehicle_cost,
    travel_time_cost,
    unassigned_requests_cost,
    vehicle_capacity_cost,
    vehicle_max_end_time_cost,
)
from optimaru.pickup_delivery.models import (
    DeliveryModel,
    PdpInput,
    PdpRequest,
    StopModel,
    VehicleModel,
)
from optimaru.pickup_delivery.models import (
    solve_request as solve_engine_request,
)

# The 100-customer Li & Lim set as published on SINTEF's TOP benchmark page
# (https://www.sintef.no/projectweb/top/pdptw/li-lim-benchmark/).
ARCHIVE_URL = "https://www.sintef.no/contentassets/1338af68996841d3922bc8e87adc430c/pdp_100.zip"
ARCHIVE_SHA256 = "d106694e9e18cebc32b63c322e1000620238392c1b9d98d92de82ef7e4b43cd9"
ARCHIVE_PREFIX = "pdp_100/"
DEFAULT_INSTANCE = "lc101"
# Li & Lim uses continuous Euclidean distance and abstract time units, while
# both local solver contracts count whole seconds. One Li & Lim unit is taken
# as ten seconds: travel is rounded to a tenth of a unit, and the 10-unit
# service time of the lr and lrc classes becomes 100 seconds. The unassigned
# penalty counts whole service minutes, so at one second per unit leaving such
# a request out would cost nothing. A larger scale (60) pushes the CP-SAT
# objective of the full lc instances past int64, where the reference model
# stops solving all levels in one pass. The printed distance is therefore a
# documented projection, not a claim of bit-identical floating-point distance
# to the published results.
DISTANCE_SCALE = 10
# Best-known solutions of the 100-customer set as listed on SINTEF's TOP page
# (https://www.sintef.no/projectweb/top/pdptw/100-customers/, read 2026-11-10):
# (vehicles, distance). They rank by fewest vehicles first, then distance, and
# their distances are unrounded, so they are a reference, not this harness's
# objective (every request served, then least distance).
BEST_KNOWN = {
    "lc101": (10, 828.94), "lc102": (10, 828.94), "lc103": (9, 1035.35),
    "lc104": (9, 860.01), "lc105": (10, 828.94), "lc106": (10, 828.94),
    "lc107": (10, 828.94), "lc108": (10, 826.44), "lc109": (9, 1000.60),
    "lc201": (3, 591.56), "lc202": (3, 591.56), "lc203": (3, 591.17),
    "lc204": (3, 590.60), "lc205": (3, 588.88), "lc206": (3, 588.49),
    "lc207": (3, 588.29), "lc208": (3, 588.32),
    "lr101": (19, 1650.80), "lr102": (17, 1487.57), "lr103": (13, 1292.68),
    "lr104": (9, 1013.39), "lr105": (14, 1377.11), "lr106": (12, 1252.62),
    "lr107": (10, 1111.31), "lr108": (9, 968.97), "lr109": (11, 1208.96),
    "lr110": (10, 1159.35), "lr111": (10, 1108.90), "lr112": (9, 1003.77),
    "lr201": (4, 1253.23), "lr202": (3, 1197.67), "lr203": (3, 949.40),
    "lr204": (2, 849.05), "lr205": (3, 1054.02), "lr206": (3, 931.63),
    "lr207": (2, 903.06), "lr208": (2, 734.85), "lr209": (3, 930.59),
    "lr210": (3, 964.22), "lr211": (2, 911.52),
    "lrc101": (14, 1708.80), "lrc102": (12, 1558.07), "lrc103": (11, 1258.74),
    "lrc104": (10, 1128.40), "lrc105": (13, 1637.62), "lrc106": (11, 1424.73),
    "lrc107": (11, 1230.14), "lrc108": (10, 1147.43),
    "lrc201": (4, 1406.94), "lrc202": (3, 1374.27), "lrc203": (3, 1089.07),
    "lrc204": (3, 818.66), "lrc205": (4, 1302.20), "lrc206": (3, 1159.03),
    "lrc207": (3, 1062.05), "lrc208": (3, 852.76),
}  # fmt: skip
ENGINE_CONSTRAINTS = [
    vehicle_capacity_cost,
    max_end_time_cost,
    travel_time_cost,
    vehicle_max_end_time_cost,
    paired_request_cost,
    same_vehicle_cost,
    pickup_before_dropoff_cost,
    unassigned_requests_cost,
]
# Pairing, same-vehicle and precedence are structural in the CP-SAT model
# (their plugins add no objective term), so only the scored counterparts are
# selected here.
CPSAT_CONSTRAINTS = [
    vehicle_capacity_cost,
    max_end_time_cost,
    travel_time_cost,
    vehicle_max_end_time_cost,
    unassigned_requests_cost,
]


@dataclass(frozen=True)
class Node:
    id: int
    x: int
    y: int
    demand: int
    ready: int
    due: int
    service: int
    pickup_id: int
    delivery_id: int


@dataclass(frozen=True)
class PdptwInstance:
    name: str
    vehicle_count: int
    capacity: int
    depot: Node
    nodes: tuple[Node, ...]

    @property
    def requests(self) -> tuple[tuple[Node, Node], ...]:
        by_id = {node.id: node for node in self.nodes}
        return tuple((node, by_id[node.delivery_id]) for node in self.nodes if node.delivery_id > 0)


def parse_instance(text: str, *, name: str = "instance") -> PdptwInstance:
    lines = [line.split() for line in text.splitlines() if line.strip()]
    if not lines or len(lines[0]) != 3:
        raise ValueError("expected Li & Lim vehicle-count/capacity/speed header")
    vehicle_count, capacity = map(int, lines[0][:2])
    rows = [Node(*map(int, row)) for row in lines[1:]]
    if not rows or rows[0].id != 0:
        raise ValueError("expected depot row 0")
    nodes = tuple(rows[1:])
    ids = {node.id for node in nodes}
    for node in nodes:
        if node.delivery_id and node.delivery_id not in ids:
            raise ValueError(f"missing delivery node {node.delivery_id}")
        if node.pickup_id and node.pickup_id not in ids:
            raise ValueError(f"missing pickup node {node.pickup_id}")
    return PdptwInstance(name, vehicle_count, capacity, rows[0], nodes)


def _download(cache_dir: Path) -> bytes:
    archive = cache_dir / "li-lim-pdp_100.zip"
    if archive.exists():
        payload = archive.read_bytes()
        if hashlib.sha256(payload).hexdigest() == ARCHIVE_SHA256:
            return payload
    request = urllib.request.Request(ARCHIVE_URL, headers={"User-Agent": "optimaru-benchmark/1.0"})
    with urllib.request.urlopen(request, timeout=60) as response:
        payload = response.read()
    digest = hashlib.sha256(payload).hexdigest()
    if digest != ARCHIVE_SHA256:
        # Nothing is cached: a transient error page must not stick.
        raise ValueError(f"{archive.name} checksum mismatch: {digest}")
    write_whole(archive, payload)
    return payload


def synthetic_instance(request_count: int, seed: int = 0) -> PdptwInstance:
    """A seeded Li & Lim-shaped instance with ``request_count`` request pairs.

    Customers sit in a 100 x 100 square, half of them in six clusters (like the
    LRC sets); service time is 90 as in the lc sets. Every request on its own
    route fits the horizon (each dropoff window opens when the pickup can be
    served and driven to directly). The fleet is one vehicle per three requests
    with capacity 200, so the solvers compete on serving everything with fewer
    vehicles and less distance; the runner reports a plan that drops requests.
    """
    if request_count < 1:
        raise ValueError("request_count must be positive")
    rng = random.Random(seed)
    horizon = 1_500 + 4 * request_count
    centers = [(rng.uniform(15, 85), rng.uniform(15, 85)) for _ in range(6)]

    def point() -> tuple[int, int]:
        if rng.random() < 0.5:
            cx, cy = rng.choice(centers)
            x, y = rng.gauss(cx, 6), rng.gauss(cy, 6)
        else:
            x, y = rng.uniform(0, 100), rng.uniform(0, 100)
        return round(min(max(x, 0), 100)), round(min(max(y, 0), 100))

    depot = Node(0, 50, 50, 0, 0, horizon, 0, 0, 0)
    nodes: list[Node] = []
    # The Li & Lim lc service time.
    service, width = 90, 150
    for index in range(request_count):
        pickup_id, dropoff_id = 2 * index + 1, 2 * index + 2
        (px, py), (dx, dy) = point(), point()
        demand = rng.randint(10, 40)
        to_pickup = math.hypot(px - 50, py - 50)
        direct = math.hypot(px - dx, py - dy)
        back = math.hypot(dx - 50, dy - 50)
        latest = int(horizon - back - direct - 2 * service - 2 * width)
        ready = rng.randint(math.ceil(to_pickup), max(math.ceil(to_pickup), latest))
        drop_ready = ready + service + math.ceil(direct)
        nodes.append(Node(pickup_id, px, py, demand, ready, ready + width, service, 0, dropoff_id))
        nodes.append(
            Node(
                dropoff_id,
                dx,
                dy,
                -demand,
                drop_ready,
                drop_ready + width,
                service,
                pickup_id,
                0,
            )
        )
    vehicles = max(2, math.ceil(request_count / 3))
    return PdptwInstance(f"synthetic-{request_count}-s{seed}", vehicles, 200, depot, tuple(nodes))


def instance_names(cache_dir: Path) -> list[str]:
    """Every instance of the archive, in name order."""
    with zipfile.ZipFile(io.BytesIO(_download(cache_dir))) as archive:
        return sorted(
            Path(item).stem for item in archive.namelist() if item.startswith(ARCHIVE_PREFIX) and item.endswith(".txt")
        )


def load_instance(name: str, cache_dir: Path) -> PdptwInstance:
    with zipfile.ZipFile(io.BytesIO(_download(cache_dir))) as archive:
        member = f"{ARCHIVE_PREFIX}{name}.txt"
        try:
            return parse_instance(archive.read(member).decode(), name=name)
        except KeyError as exc:
            raise ValueError(f"unknown instance {name!r}; available: {', '.join(instance_names(cache_dir))}") from exc


def _distance(left: Node, right: Node) -> int:
    return round(math.hypot(left.x - right.x, left.y - right.y) * DISTANCE_SCALE)


def make_request(
    instance: PdptwInstance,
    *,
    seconds: float,
    seed: int,
    workers: int,
    request_limit: int | None = None,
) -> tuple[PdpRequest, tuple[tuple[Node, Node], ...]]:
    requests = instance.requests[:request_limit]
    if not requests:
        raise ValueError("request_limit selected no requests")
    stop_nodes = tuple(node for pair in requests for node in pair)
    vehicle_count = min(instance.vehicle_count, len(requests))
    ordered = (instance.depot,) * vehicle_count + stop_nodes
    driving = [[_distance(left, right) for right in ordered] for left in ordered]
    depot_location = (instance.depot.y / 1_000, instance.depot.x / 1_000)
    return (
        PdpRequest(
            name=instance.name,
            timelimit=seconds,
            random_seed=seed,
            num_workers=workers,
            model_input=PdpInput(
                vehicles=[
                    VehicleModel(
                        id=f"V{i + 1}",
                        capacity=instance.capacity,
                        home=depot_location,
                        max_end_time=instance.depot.due * DISTANCE_SCALE,
                        # Li & Lim has no stop cap: lift the contract's default.
                        max_stops=0,
                        # Li & Lim scores the round trip and the depot's due
                        # time applies to the return, so close every route.
                        return_to_depot=True,
                    )
                    for i in range(vehicle_count)
                ],
                deliveries=[
                    DeliveryModel(
                        id=str(pickup.id),
                        demand=pickup.demand,
                        pickup=StopModel(
                            id=str(pickup.id),
                            location=(pickup.y / 1_000, pickup.x / 1_000),
                            min_start_time=pickup.ready * DISTANCE_SCALE,
                            max_end_time=(pickup.due + pickup.service) * DISTANCE_SCALE,
                            service_duration=pickup.service * DISTANCE_SCALE,
                        ),
                        dropoff=StopModel(
                            id=str(dropoff.id),
                            location=(dropoff.y / 1_000, dropoff.x / 1_000),
                            min_start_time=dropoff.ready * DISTANCE_SCALE,
                            max_end_time=(dropoff.due + dropoff.service) * DISTANCE_SCALE,
                            service_duration=dropoff.service * DISTANCE_SCALE,
                        ),
                    )
                    for pickup, dropoff in requests
                ],
                driving_seconds=driving,
            ),
        ),
        requests,
    )


class IncompletePlan(ValueError):
    """The routes leave a selected stop out (or serve one twice)."""


def rescore(
    instance: PdptwInstance,
    requests: tuple[tuple[Node, Node], ...],
    routes: dict[str, list],
) -> tuple[int, int]:
    """Recheck coverage, pairing, precedence, capacity and every time window."""
    selected = {node.id: node for pair in requests for node in pair}
    pickup_ids = {pickup.id for pickup, _ in requests}
    delivery_to_pickup = {dropoff.id: pickup.id for pickup, dropoff in requests}
    seen: list[int] = []
    used = 0
    distance = 0
    for stops in routes.values():
        ids = [int(stop.stop_id) for stop in stops]
        if not ids:
            continue
        used += 1
        load = 0
        clock = 0
        previous = instance.depot
        picked: set[int] = set()
        for node_id in ids:
            node = selected[node_id]
            distance += _distance(previous, node)
            clock += _distance(previous, node)
            clock = max(clock, node.ready * DISTANCE_SCALE)
            clock += node.service * DISTANCE_SCALE
            if clock > (node.due + node.service) * DISTANCE_SCALE:
                raise ValueError(f"time window violated at node {node_id}")
            if node_id in pickup_ids:
                load += node.demand
                picked.add(node_id)
            else:
                pickup_id = delivery_to_pickup[node_id]
                if pickup_id not in picked:
                    raise ValueError(f"delivery {node_id} precedes pickup {pickup_id}")
                load += node.demand
            if not 0 <= load <= instance.capacity:
                raise ValueError(f"capacity violated at node {node_id}")
            seen.append(node_id)
            previous = node
        distance += _distance(previous, instance.depot)
        clock += _distance(previous, instance.depot)
        if clock > instance.depot.due * DISTANCE_SCALE:
            raise ValueError("vehicle returned after the depot horizon")
    if sorted(seen) != sorted(selected):
        raise IncompletePlan("returned routes do not cover every selected stop exactly once")
    return used, distance


def _run(backend: str, request: PdpRequest):
    started = time.perf_counter()
    if backend == "engine":
        # The engine always runs single-threaded; `--workers` is CP-SAT's.
        solution = solve_engine_request(
            request.model_copy(update={"num_workers": 1}),
            constraints=ENGINE_CONSTRAINTS,
        )
    else:
        solution = solve_cpsat_request(request, constraints=CPSAT_CONSTRAINTS)
    return solution, time.perf_counter() - started


SOLVER_NAMES = {"engine": "engine", "cpsat": "cp-sat"}


def run_case(
    instance: PdptwInstance,
    request_limit: int | None,
    *,
    seconds: float,
    seed: int,
    workers: int,
    backends: tuple[str, ...],
    log=print,
) -> dict:
    """Solve one instance with every backend and re-verify each plan.

    A run without a usable plan keeps ``vehicles`` and ``distance`` at None and
    says why in ``status``: ``no solution: ...`` (the solver returned nothing in
    time) or ``rejected: ...`` (the plan is incomplete or breaks a rule).
    """
    request, selected = make_request(
        instance,
        seconds=seconds,
        seed=seed,
        workers=workers,
        request_limit=request_limit,
    )
    log(
        f"{instance.name}: {len(selected)}/{len(instance.requests)} requests, "
        f"{len(request.model_input.vehicles)} vehicles, capacity {instance.capacity}"
    )
    runs = []
    for backend in backends:
        run = {
            "solver": SOLVER_NAMES[backend],
            "vehicles": None,
            "distance": None,
            "seconds": None,
            "score": None,
            # ok, no solution, incomplete, infeasible or mismatch; `status` has the words.
            "outcome": "ok",
            "status": "",
        }
        runs.append(run)
        started = time.perf_counter()
        try:
            solution, elapsed = _run(backend, request)
        except RuntimeError as exc:  # e.g. CP-SAT found no solution in time
            run["seconds"] = time.perf_counter() - started
            run["outcome"] = "no solution"
            run["status"] = f"no solution: {exc}"
            log(f"{backend:6} no solution: {exc}")
            continue
        run["seconds"] = elapsed
        run["score"] = str(solution.score)
        try:
            used, distance = rescore(instance, selected, solution.routes)
        except ValueError as exc:  # incomplete or rule-breaking plan
            run["outcome"] = "incomplete" if isinstance(exc, IncompletePlan) else "infeasible"
            run["status"] = f"rejected: {exc}"
            log(f"{backend:6} rejected: {exc} elapsed={elapsed:.2f}s score={solution.score}")
            continue
        if solution.total_driving_seconds != distance:
            # A harness or adapter bug, not a result. Recorded, so a sweep of many
            # instances keeps what it has measured; the publisher refuses it.
            run["outcome"] = "mismatch"
            run["status"] = (
                f"mismatch: the solver reports {solution.total_driving_seconds}, the independent scorer {distance}"
            )
            log(f"{backend:6} {run['status']}")
            continue
        run["vehicles"] = used
        run["distance"] = distance / DISTANCE_SCALE
        log(
            f"{backend:6} used={used} distance={distance / DISTANCE_SCALE:.3f} "
            f"elapsed={elapsed:.2f}s score={solution.score}"
        )
    # The published best-known solution covers the whole instance only.
    reference = BEST_KNOWN.get(instance.name) if len(selected) == len(instance.requests) else None
    return {
        "instance": instance.name,
        "requests": len(selected),
        "total_requests": len(instance.requests),
        "vehicles": len(request.model_input.vehicles),
        "capacity": instance.capacity,
        "best_known": (None if reference is None else {"vehicles": reference[0], "distance": reference[1]}),
        # What the run was given, so a table can state it and a publisher can check it.
        "budget": seconds,
        "seed": seed,
        "workers": workers,
        "runs": runs,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--instance",
        action="append",
        default=None,
        help=f"Li & Lim instance stem; repeat to select several (default {DEFAULT_INSTANCE})",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="run every instance of the archive, whole unless --requests says otherwise",
    )
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument(
        "--workers",
        type=int,
        default=1,
        help="CP-SAT search workers; the engine always runs single-threaded",
    )
    parser.add_argument(
        "--requests",
        type=int,
        action="append",
        default=None,
        help="request pairs taken from each official instance (default 10, or the full file "
        "with --all); repeat for several sizes; use 0 for the full file",
    )
    parser.add_argument(
        "--synthetic",
        type=int,
        action="append",
        default=None,
        metavar="N",
        help="solve a seeded synthetic instance with N requests instead (all of them); repeat for several sizes",
    )
    parser.add_argument("--backend", choices=("engine", "cpsat", "both"), default="both")
    parser.add_argument("--cache-dir", type=Path, default=Path(__file__).with_name(".cache"))
    parser.add_argument(
        "--json",
        action="store_true",
        dest="as_json",
        help="print the results as JSON (progress goes to stderr)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="write the results as JSON here after every instance, so an interrupted run keeps them",
    )
    args = parser.parse_args(argv)
    if sum(bool(choice) for choice in (args.all, args.instance, args.synthetic)) > 1:
        parser.error("--all, --instance and --synthetic are mutually exclusive")
    if args.synthetic and args.requests:
        parser.error("--requests cuts an official instance; a synthetic instance is run whole")

    cases: list[tuple[PdptwInstance, int | None]]
    if args.synthetic:
        cases = [(synthetic_instance(count, args.seed), None) for count in args.synthetic]
    else:
        names = instance_names(args.cache_dir) if args.all else (args.instance or [DEFAULT_INSTANCE])
        cases = [
            (load_instance(name, args.cache_dir), limit or None)
            for name in names
            # The whole archive is run on whole instances unless a size is asked for.
            for limit in (args.requests or ([0] if args.all else [10]))
        ]
    backends = ("engine", "cpsat") if args.backend == "both" else (args.backend,)

    def log(line: str) -> None:
        print(line, file=sys.stderr if args.as_json else sys.stdout, flush=True)

    log(f"objective: assign every request, then minimize distance (rounded to 1/{DISTANCE_SCALE} of a unit)")
    results = []
    for instance, limit in cases:
        results.append(
            run_case(
                instance,
                limit,
                seconds=args.seconds,
                seed=args.seed,
                workers=args.workers,
                backends=backends,
                log=log,
            )
        )
        if args.out is not None:
            write_results(args.out, results)
    if args.as_json:
        print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
