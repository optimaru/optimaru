"""CVRP instances and the solver-independent rescoring every backend goes through."""

from __future__ import annotations

import hashlib
import io
import itertools
import math
import re
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

# Augerat sets A/B/P with their optimum in the instance comment.
ARCHIVE_URL = "https://www.hexaly.com/examples/cvrp.zip"
ARCHIVE_SHA256 = "8deedef8bee61002c0a8b3c176a6f8d41db8a9b283ef51f9411776ae850c2474"


@dataclass(frozen=True)
class CvrpInstance:
    """A CVRPLIB EUC_2D instance. Node 0 is the depot, 1..n the customers."""

    name: str
    vehicle_count: int
    capacity: int
    coordinates: tuple[tuple[int, int], ...]
    demands: tuple[int, ...]
    # Rounded Euclidean distances (CVRPLIB's EUC_2D rule), the one matrix
    # every backend is given and every result is rescored with.
    distances: tuple[tuple[int, ...], ...]
    best_known_distance: int | None
    proven_optimal: bool

    @property
    def customer_count(self) -> int:
        return len(self.demands) - 1


@dataclass(frozen=True)
class Rescore:
    used_vehicles: int
    distance: int
    unassigned: int
    # False when a route exceeds the vehicle capacity: a backend that cannot
    # leave a customer out returns an over-capacity plan instead.
    feasible: bool


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
            proven_optimal if proven_optimal is not None else comment_optimum is not None
        ),
    )


def _download_archive(cache_dir: Path) -> bytes:
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


def load_augerat_instance(name: str, cache_dir: Path) -> CvrpInstance:
    """One Augerat instance (e.g. A-n32-k5) from the checksummed archive."""
    with zipfile.ZipFile(io.BytesIO(_download_archive(cache_dir))) as archive:
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


def rescore(instance: CvrpInstance, routes: list[list[int]]) -> Rescore:
    """Recompute a backend's routes from scratch.

    `routes` lists customer nodes (1..n) per vehicle, depot excluded; every
    route is a round trip from and back to the depot, as CVRPLIB scores it.
    Raises on a duplicate or unknown customer or on more routes than vehicles:
    those are harness or adapter bugs, not solution quality.
    """
    used = [nodes for nodes in routes if nodes]
    if len(used) > instance.vehicle_count:
        raise ValueError(
            f"{len(used)} routes for a fleet of {instance.vehicle_count} vehicles"
        )
    seen = [node for nodes in used for node in nodes]
    if len(seen) != len(set(seen)):
        raise ValueError("returned routes visit a customer more than once")
    expected = set(range(1, len(instance.demands)))
    if not set(seen) <= expected:
        raise ValueError("returned routes contain an unknown customer")
    distance = 0
    feasible = True
    for nodes in used:
        if sum(instance.demands[node] for node in nodes) > instance.capacity:
            feasible = False
        path = [0, *nodes, 0]
        distance += sum(
            instance.distances[left][right] for left, right in itertools.pairwise(path)
        )
    return Rescore(
        used_vehicles=len(used),
        distance=distance,
        unassigned=len(expected - set(seen)),
        feasible=feasible,
    )
