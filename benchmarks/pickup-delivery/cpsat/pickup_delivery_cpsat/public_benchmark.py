"""Compare both backends on Li & Lim PDPTW instances (Hexaly bundle or synthetic)."""

from __future__ import annotations

import argparse
import hashlib
import io
import math
import random
import time
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

from pdp_routing.constraints import (
    minimize_travel_time,
    paired_request,
    pickup_before_dropoff,
    same_vehicle,
    service_finished_after_max_end_time,
    unassigned_requests,
    vehicle_capacity,
    vehicle_end_time,
)
from pdp_routing.models import (
    DeliveryModel,
    PdpInput,
    PdpRequest,
    StopModel,
    VehicleModel,
)
from pdp_routing.models import (
    solve_request as solve_engine_request,
)

from .solve import solve_request as solve_cpsat_request

ARCHIVE_URL = "https://www.hexaly.com/examples/pdptw.zip"
ARCHIVE_SHA256 = "d396d712e879edb73adcdaa420e2c13bcc6b086d83f790a404692faae49dd26e"
DEFAULT_INSTANCE = "lc101"
# Li & Lim uses continuous Euclidean distance while both local solver
# contracts are integral. Nearest-unit travel is the shared comparison metric;
# the independently printed objective therefore is a documented projection,
# not a claim of bit-identical floating-point distance to Hexaly.
DISTANCE_SCALE = 1
ENGINE_CONSTRAINTS = [
    vehicle_capacity,
    service_finished_after_max_end_time,
    minimize_travel_time,
    vehicle_end_time,
    paired_request,
    same_vehicle,
    pickup_before_dropoff,
    unassigned_requests,
]
# Pairing, same-vehicle and precedence are structural CP-SAT constraints, not
# objective plugins, so only their scored counterparts are selected here.
CPSAT_CONSTRAINTS = [
    vehicle_capacity,
    service_finished_after_max_end_time,
    minimize_travel_time,
    vehicle_end_time,
    unassigned_requests,
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
        return tuple(
            (node, by_id[node.delivery_id])
            for node in self.nodes
            if node.delivery_id > 0
        )


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
    cache_dir.mkdir(parents=True, exist_ok=True)
    archive = cache_dir / "pdptw.zip"
    if not archive.exists():
        with urllib.request.urlopen(ARCHIVE_URL, timeout=60) as response:
            archive.write_bytes(response.read())
    payload = archive.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if digest != ARCHIVE_SHA256:
        raise ValueError(f"pdptw.zip checksum mismatch: {digest}")
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
    # Li & Lim lc service time; the unassigned penalty counts whole service
    # minutes, so a short service would make dropping a request free.
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
            Node(dropoff_id, dx, dy, -demand, drop_ready, drop_ready + width, service, pickup_id, 0)
        )
    vehicles = max(2, math.ceil(request_count / 3))
    return PdptwInstance(f"synthetic-{request_count}-s{seed}", vehicles, 200, depot, tuple(nodes))


def load_instance(name: str, cache_dir: Path) -> PdptwInstance:
    with zipfile.ZipFile(io.BytesIO(_download(cache_dir))) as archive:
        member = f"pdptw/instances/{name}.txt"
        try:
            return parse_instance(archive.read(member).decode(), name=name)
        except KeyError as exc:
            available = sorted(
                Path(item).stem
                for item in archive.namelist()
                if item.startswith("pdptw/instances/") and item.endswith(".txt")
            )
            raise ValueError(
                f"unknown instance {name!r}; available: {', '.join(available)}"
            ) from exc


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
                            max_end_time=(dropoff.due + dropoff.service)
                            * DISTANCE_SCALE,
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
        raise ValueError(
            "returned routes do not cover every selected stop exactly once"
        )
    return used, distance


def _run(backend: str, request: PdpRequest):
    started = time.perf_counter()
    if backend == "engine":
        solution = solve_engine_request(request, constraints=ENGINE_CONSTRAINTS)
    else:
        solution = solve_cpsat_request(request, constraints=CPSAT_CONSTRAINTS)
    return solution, time.perf_counter() - started


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", default=DEFAULT_INSTANCE)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument(
        "--requests",
        type=int,
        default=10,
        help="request pairs from the official instance; use 0 for the full file",
    )
    parser.add_argument(
        "--synthetic",
        type=int,
        default=0,
        metavar="N",
        help="solve a seeded synthetic instance with N requests instead (all of them)",
    )
    parser.add_argument(
        "--backend", choices=("engine", "cpsat", "both"), default="both"
    )
    parser.add_argument(
        "--cache-dir", type=Path, default=Path(__file__).with_name(".cache")
    )
    args = parser.parse_args(argv)

    if args.synthetic:
        instance = synthetic_instance(args.synthetic, args.seed)
        limit = None
    else:
        instance = load_instance(args.instance, args.cache_dir)
        limit = args.requests or None
    request, selected = make_request(
        instance,
        seconds=args.seconds,
        seed=args.seed,
        workers=args.workers,
        request_limit=limit,
    )
    print(
        f"{instance.name}: {len(selected)}/{len(instance.requests)} requests, "
        f"{len(request.model_input.vehicles)} vehicles, capacity {instance.capacity}"
    )
    print("objective: assign every request, then minimize integral distance")
    backends = ("engine", "cpsat") if args.backend == "both" else (args.backend,)
    for backend in backends:
        try:
            solution, elapsed = _run(backend, request)
        except RuntimeError as exc:  # e.g. CP-SAT found no solution in time
            print(f"{backend:6} no solution: {exc}")
            continue
        try:
            used, distance = rescore(instance, selected, solution.routes)
        except ValueError as exc:  # incomplete or rule-breaking plan
            print(f"{backend:6} rejected: {exc} elapsed={elapsed:.2f}s score={solution.score}")
            continue
        if solution.total_driving_seconds != distance:
            raise ValueError("solver report and independent distance disagree")
        print(
            f"{backend:6} used={used} distance={distance / DISTANCE_SCALE:.3f} "
            f"elapsed={elapsed:.2f}s score={solution.score}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
