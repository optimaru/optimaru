"""Solver adapters. Each takes the same `CvrpInstance` (fleet size, capacity,
rounded integer distance matrix) and wall-clock budget, and returns routes as
customer nodes per vehicle for the harness to rescore."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

from ..instance import CvrpInstance


@dataclass(frozen=True)
class SolverOutcome:
    routes: list[list[int]]  # customer nodes (1..n) per vehicle, depot excluded
    # The solver's own objective, checked against the harness's rescore when
    # the backend reports a plain distance; free text otherwise.
    reported_distance: int | None
    summary: str


Solve = Callable[[CvrpInstance, float, int], SolverOutcome]


def get(name: str) -> Solve:
    """Import an adapter lazily, so a missing optional dependency (Java for
    Timefold) only matters when that backend is asked for."""
    if name == "engine":
        from .engine import solve
    elif name == "ortools":
        from .ortools_routing import solve
    elif name == "timefold":
        from .timefold import solve
    else:
        raise ValueError(f"unknown backend {name!r}; expected one of {BACKENDS}")
    return solve


BACKENDS = ("engine", "ortools", "timefold")
