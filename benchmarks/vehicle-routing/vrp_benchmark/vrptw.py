"""VRPTW instances: Solomon (1987) and Gehring & Homberger (1999).

Solomon's 56 instances have 100 customers in six classes: C (clustered), R
(random) and RC (mixed) customers, each with a short horizon and small
vehicles, so few customers per route (C1, R1, RC1), and with a long horizon
and large vehicles (C2, R2, RC2).
Gehring and Homberger extended the same six classes to 200, 400, 600, 800 and
1000 customers, ten instances per class and size (300 instances).

The scoring follows the DIMACS convention of the 12th DIMACS Implementation
Challenge (VRPTW track), under which CVRPLIB lists its best-known solutions:
the objective is total distance alone (not the fewest vehicles first), and
every distance, which is also the travel time, is the Euclidean distance
truncated to one decimal. The harness keeps every distance and time in
tenths, so all of them are integers (`scale` 10).

The files come from the PyVRP/Instances GitHub repository, which carries all
356 instances in VRPLIB form and, in each `.sol`, the CVRPLIB best-known
solution under that convention. Downloads are pinned to one commit of it, so
an instance and its best-known value cannot change under a run; the instance
files are not checksummed one by one, the commit hash stands for them.
"""

from __future__ import annotations

import math
import os
import re
import urllib.request
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path

from .instance import CvrpInstance

MIRROR_COMMIT = "7474b068a8effa7f39448b241314b615c9271525"
MIRROR_BASE_URL = f"https://raw.githubusercontent.com/PyVRP/Instances/{MIRROR_COMMIT}/VRPTW/"
SCALE = 10
CLASSES = ("C1", "C2", "R1", "R2", "RC1", "RC2")
# Solomon's instances per class: C101-C109, C201-C208, R101-R112, R201-R211,
# RC101-RC108, RC201-RC208.
SOLOMON_COUNTS = {"C1": 9, "C2": 8, "R1": 12, "R2": 11, "RC1": 8, "RC2": 8}
HOMBERGER_SIZES = (200, 400, 600, 800, 1000)


@dataclass(frozen=True)
class VrptwEntry:
    name: str
    series: str  # "solomon" or "homberger"
    instance_class: str  # C1 ... RC2
    customer_count: int

    @property
    def path(self) -> str:
        """The file's path under the mirror's VRPTW directory, without suffix."""
        if self.series == "solomon":
            return f"Solomon/{self.name}"
        if self.customer_count == 1000:
            return self.name
        return f"GH{self.customer_count}/{self.name}"


def solomon_entries() -> tuple[VrptwEntry, ...]:
    return tuple(
        VrptwEntry(f"{cls}{index:02d}", "solomon", cls, 100)
        for cls in CLASSES
        for index in range(1, SOLOMON_COUNTS[cls] + 1)
    )


def homberger_entries() -> tuple[VrptwEntry, ...]:
    """Smallest first, then by class and number: C1_2_1, ..., RC2_10_10."""
    return tuple(
        VrptwEntry(f"{cls}_{size // 100}_{index}", "homberger", cls, size)
        for size in HOMBERGER_SIZES
        for cls in CLASSES
        for index in range(1, 11)
    )


def dimacs_distance(a: tuple[int, int], b: tuple[int, int]) -> int:
    """Euclidean distance truncated to one decimal, in tenths (integer coordinates)."""
    dx, dy = a[0] - b[0], a[1] - b[1]
    return math.isqrt(100 * (dx * dx + dy * dy))


def parse_solution_cost(text: str) -> int:
    """The `Cost` line of a `.sol` file, in tenths."""
    match = re.search(r"^\s*Cost\s+([0-9]+(?:\.[0-9]+)?)\s*$", text, re.I | re.M)
    if match is None:
        raise ValueError("solution file carries no Cost line")
    tenths = Decimal(match.group(1)) * SCALE
    if tenths != tenths.to_integral_value():
        raise ValueError(f"cost {match.group(1)} has more than one decimal")
    return int(tenths)


def parse_solution_routes(text: str) -> list[list[int]]:
    """The `Route #k:` lines of a `.sol` file, customers numbered 1..n."""
    return [
        [int(token) for token in line.split(":", 1)[1].split()]
        for line in text.splitlines()
        if line.strip().lower().startswith("route")
    ]


def parse_vrptw(text: str, *, best_known_distance: int | None = None) -> CvrpInstance:
    """Parse a VRPLIB VRPTW file: EUC_2D, depot node 1, one service time for all customers.

    The fleet is the file's VEHICLES; a plan may leave vehicles unused.
    """
    headers: dict[str, str] = {}
    sections: dict[str, list[list[int]]] = {}
    section: str | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line == "EOF":
            continue
        if line.endswith("_SECTION"):
            section = line
            sections[section] = []
        elif section is not None:
            sections[section].append([int(token) for token in line.split()])
        elif ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip()] = value.strip()
    if headers.get("EDGE_WEIGHT_TYPE") != "EUC_2D":
        raise ValueError("only EUC_2D VRPTW instances are supported")
    dimension = int(headers["DIMENSION"])

    def column(name: str, width: int) -> list[list[int]]:
        rows = sorted(sections[name])
        if [row[0] for row in rows] != list(range(1, dimension + 1)) or any(len(row) != width for row in rows):
            raise ValueError(f"{name} does not list every node once")
        return [row[1:] for row in rows]

    coordinates = tuple((row[0], row[1]) for row in column("NODE_COORD_SECTION", 3))
    demands = tuple(row[0] for row in column("DEMAND_SECTION", 2))
    windows = column("TIME_WINDOW_SECTION", 3)
    depot_tokens = [token for row in sections["DEPOT_SECTION"] for token in row]
    if depot_tokens != [1, -1] or demands[0] != 0:
        raise ValueError("expected one depot at node 1 with no demand")
    if any(ready > due for ready, due in windows):
        raise ValueError("a time window opens after it closes")
    service = int(headers["SERVICE_TIME"]) * SCALE
    return CvrpInstance(
        name=headers["NAME"],
        vehicle_count=int(headers["VEHICLES"]),
        capacity=int(headers["CAPACITY"]),
        coordinates=coordinates,
        demands=demands,
        distances=tuple(tuple(dimacs_distance(a, b) for b in coordinates) for a in coordinates),
        best_known_distance=best_known_distance,
        # CVRPLIB proves some of these optimal; the mirror does not say which.
        proven_optimal=False,
        ready_times=tuple(ready * SCALE for ready, _ in windows),
        due_times=tuple(due * SCALE for _, due in windows),
        service_times=(0,) + (service,) * (dimension - 1),
        scale=SCALE,
    )


def _download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "optimaru-benchmark/1.0"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def _cached(url: str, path: Path, check) -> str:
    """The file at `url`, cached at `path` once `check` accepts it.

    A cached copy that `check` refuses (an error page, a truncated write) is
    fetched again; a fresh download it refuses is an error, and is not kept.
    """
    if path.exists():
        text = path.read_text()
        try:
            check(text)
            return text
        except (ValueError, KeyError):
            # KeyError: a header or section missing from a damaged copy.
            pass
    payload = _download(url)
    text = payload.decode()
    check(text)
    path.parent.mkdir(parents=True, exist_ok=True)
    # A name of this process: parallel runs that start on an empty cache download the same file at once.
    partial = path.with_name(f"{path.name}.{os.getpid()}.part")
    partial.write_bytes(payload)
    partial.replace(path)
    return text


def load_vrptw(entry: VrptwEntry, cache_dir: Path) -> CvrpInstance:
    """An instance with its best-known distance, downloaded from the pinned mirror commit and cached."""
    base = cache_dir / "vrptw" / MIRROR_COMMIT[:12] / entry.name
    solution = _cached(f"{MIRROR_BASE_URL}{entry.path}.sol", base.with_suffix(".sol"), parse_solution_cost)
    text = _cached(f"{MIRROR_BASE_URL}{entry.path}.vrp", base.with_suffix(".vrp"), parse_vrptw)
    instance = parse_vrptw(text, best_known_distance=parse_solution_cost(solution))
    if instance.name != entry.name or instance.customer_count != entry.customer_count:
        raise ValueError(f"{entry.path}: expected {entry.name} with {entry.customer_count} customers")
    return instance
