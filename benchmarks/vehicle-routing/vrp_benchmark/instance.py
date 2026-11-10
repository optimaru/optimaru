"""CVRP instances and the solver-independent rescoring every backend goes through."""

from __future__ import annotations

import hashlib
import itertools
import math
import os
import re
import urllib.request
from dataclasses import dataclass
from pathlib import Path

# Augerat et al. (1995) Set A, downloaded file by file from CVRPLIB; each file
# carries its optimum in the instance comment. CVRPLIB serves a single
# instance by its numeric catalog id.
CVRPLIB_INSTANCE_URL = "https://galgos.inf.puc-rio.br/cvrplib/index.php/en/download/instance/{}"
AUGERAT_A = {
    "A-n32-k5": (4, "2ebedd0631c4c56c08ba3b1476ac874a7d54cc448bd3795de534f43b2c099ce2"),
    "A-n33-k5": (5, "35712de0054a0fbfd609d5cdb4ec638c39f7c181b6e39e3c8bf6f8e359b78aef"),
    "A-n33-k6": (7, "4d01ebe9842743ee0c66f14ecb86ab9c84d3198b3280d0dbcc8a8d9a69f1592f"),
    "A-n34-k5": (8, "393117f10a26d0e803a8206e7dd1157a67de2429d6b9fd2d60c070a98b7c73bb"),
    "A-n36-k5": (9, "794360562d60d66a358eaf57721432b43318abba710f3c55f3240aef18e69b03"),
    "A-n37-k5": (10, "8b3c9c86c6ec147d6699ef1a7e9b33d7d2637c6b8f254922ff3494d56efed3d0"),
    "A-n37-k6": (11, "65615d1b371722c00089443548bff64a5b6385055eeac3ec6f0b5c8cc8c9bdab"),
    "A-n38-k5": (12, "59a822563b0c59b86f0652e9249e6073cb1f22f19b30da45554ca47055e91051"),
    "A-n39-k5": (13, "d3afcea200df090c673805d02d4f3c3fad84f4ebaee26a54edc28b30a72cbac9"),
    "A-n39-k6": (14, "28b83a6afdaa1a2f1f6691313b8977ac3003201584ccb8387fa5ac91ee195cfe"),
    "A-n44-k6": (15, "4303b6c5ad8c5f09901808bd72e7e8920cc81e4b1f725900cf9a63e0bfbb0bec"),
    "A-n45-k6": (16, "b3e7a3290b732e2140aa180b0b8a83b4dcccc6297045af270c50f62edcb15f31"),
    "A-n45-k7": (17, "ab22ac5adcd03923127d41c394fd32971ecae0a7d6854c148bfb430dc1b462de"),
    "A-n46-k7": (18, "843d960f050ed93387dad6ead572f647a631ea90f1dc15879740c98ed7211060"),
    "A-n48-k7": (19, "0457ce2d3921efb0e55b248517df2b77c061474fe120b9a07b382cc3a884845c"),
    "A-n53-k7": (20, "7a22295b926b50f54ecca7da21924b942ef60949f7707b071936c7cdf3d9006c"),
    "A-n54-k7": (21, "a85afa8f15544b168ce3413d759a5c4f5fbfef5e1f7ea0972839c2761c958438"),
    "A-n55-k9": (22, "0ec4706e6efb69217bc11ea4942edd12626f4c41bd6e6b0227aff1035c8f6eda"),
    "A-n60-k9": (23, "d60d067db572ee49d265be1f01f5f03b88bb8fb543308d96de1b913408a67de7"),
    "A-n61-k9": (24, "210b68819f2f8b1a525b5415fa3559f3d23b90e119ef79a2f16d638f9ad884ab"),
    "A-n62-k8": (25, "06c2ee55bb37e610d7772e93c1d92e0679f6baca5b878469a954d8da3eb803d5"),
    "A-n63-k9": (26, "4bb0410d5e0cee22f9a0af7854b1e09652314e75acb48703250b33277b0a7033"),
    "A-n63-k10": (27, "d837c6c264249b2c1bf643cdb113785bf3b0e6eb9596539d1cb705454a61724b"),
    "A-n64-k9": (28, "1d690e428ea2de12f5996cf1e84aaa0508ec9977dfdb3d0c106e9006e9cc5124"),
    "A-n65-k9": (29, "2f2768011d0dc41fb912ad3d4a8c34678334607bf0263cf4cd20b6c1b4ab4cd6"),
    "A-n69-k9": (30, "7b9bd50d901639c15a9f4ff1cf38ccef7a5bf050697b4e6796ff07bc95920c64"),
    "A-n80-k10": (31, "f74d5d9f8e24245d94cad53ac20892a70b554aa7559f9dfcd962c8f83a68a410"),
}


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


def free_fleet(demands: tuple[int, ...], capacity: int) -> int:
    """The vehicles a plan may use on a set that does not fix the fleet.

    Set X leaves the number of routes open: the `-kN` of a name is the fewest
    vehicles that can carry the demand, and the best-known solutions of 22 of
    its 100 instances use more. Every backend therefore gets a fleet that
    cannot bind, 30% above the capacity lower bound plus three vehicles, and
    is free to leave vehicles unused.
    """
    return math.ceil(1.3 * sum(demands) / capacity) + 3


def parse_instance(
    text: str,
    *,
    best_known_distance: int | None = None,
    proven_optimal: bool | None = None,
    fixed_fleet: bool = True,
) -> CvrpInstance:
    """Parse a CVRPLIB EUC_2D instance with depot node 1.

    The fleet is the `-kN` of the instance name (the classical sets, such as
    Augerat's, fix the number of routes), or `free_fleet()` with
    `fixed_fleet=False` (Set X).
    """
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
        int(parts[0]): int(parts[1]) for line in sections["DEMAND_SECTION"] if len(parts := line.split()) == 2
    }
    depot_tokens = [int(token) for line in sections["DEPOT_SECTION"] for token in line.split()]
    if depot_tokens != [1, -1] or len(coordinates_by_id) != dimension:
        raise ValueError("expected one depot at node 1 and a complete coordinate set")
    coordinates = tuple(coordinates_by_id[i] for i in range(1, dimension + 1))
    demands = tuple(demands_by_id[i] for i in range(1, dimension + 1))
    if demands[0] != 0:
        raise ValueError("depot demand must be zero")
    distances = tuple(tuple(_rounded_euclidean(left, right) for right in coordinates) for left in coordinates)
    name = headers["NAME"]
    vehicle_match = re.search(r"-k(\d+)$", name)
    if vehicle_match is None:
        raise ValueError(f"instance name does not carry the fleet size: {name}")
    optimum_match = re.search(r"Optimal value:\s*(\d+)", headers.get("COMMENT", ""))
    comment_optimum = int(optimum_match.group(1)) if optimum_match else None
    capacity = int(headers["CAPACITY"])
    return CvrpInstance(
        name=name,
        vehicle_count=(int(vehicle_match.group(1)) if fixed_fleet else free_fleet(demands, capacity)),
        capacity=capacity,
        coordinates=coordinates,
        demands=demands,
        distances=distances,
        best_known_distance=(best_known_distance if best_known_distance is not None else comment_optimum),
        proven_optimal=(proven_optimal if proven_optimal is not None else comment_optimum is not None),
    )


def load_augerat_instance(name: str, cache_dir: Path) -> CvrpInstance:
    """One Augerat Set A instance (e.g. A-n32-k5), cached only once its SHA-256 matches."""
    try:
        catalog_id, expected = AUGERAT_A[name]
    except KeyError as exc:
        raise ValueError(f"unknown instance {name!r}; available: {', '.join(sorted(AUGERAT_A))}") from exc
    path = cache_dir / "cvrplib-a" / f"{name}.vrp"
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == expected:
        return parse_instance(path.read_text())
    url = CVRPLIB_INSTANCE_URL.format(catalog_id)
    request = urllib.request.Request(url, headers={"User-Agent": "optimaru-benchmark/1.0"})
    with urllib.request.urlopen(request, timeout=60) as response:
        payload = response.read()
    digest = hashlib.sha256(payload).hexdigest()
    if digest != expected:
        # Nothing is cached: a transient error page must not stick.
        raise ValueError(f"{name}.vrp checksum mismatch for {url}: {digest}")
    path.parent.mkdir(parents=True, exist_ok=True)
    # A name of this process: lanes that start on an empty cache download the same file at once.
    partial = path.with_name(f"{path.name}.{os.getpid()}.part")
    partial.write_bytes(payload)
    partial.replace(path)
    return parse_instance(payload.decode())


def rescore(instance: CvrpInstance, routes: list[list[int]]) -> Rescore:
    """Recompute a backend's routes from scratch.

    `routes` lists customer nodes (1..n) per vehicle, depot excluded; every
    route is a round trip from and back to the depot, as CVRPLIB scores it.
    Raises on a duplicate or unknown customer or on more routes than vehicles:
    those are harness or adapter bugs, not solution quality.
    """
    used = [nodes for nodes in routes if nodes]
    if len(used) > instance.vehicle_count:
        raise ValueError(f"{len(used)} routes for a fleet of {instance.vehicle_count} vehicles")
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
        distance += sum(instance.distances[left][right] for left, right in itertools.pairwise(path))
    return Rescore(
        used_vehicles=len(used),
        distance=distance,
        unassigned=len(expected - set(seen)),
        feasible=feasible,
    )
