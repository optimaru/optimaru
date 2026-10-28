"""Shared class-based score plugins for bin packing."""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "BinCapacity",
    "ColorCompatibility",
    "Constraint",
    "ItemConflicts",
    "MinimizeBins",
    "Score",
    "bin_capacity",
    "get_constraints",
    "minimize_bins",
    "score",
]


@dataclass(frozen=True)
class Constraint:
    """One selectable penalty with weight, score level, and shaping."""

    weight: float | None = None
    hard: bool | None = None
    exponent: float = 1.0
    name: ClassVar[str] = ""

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} must set a registry name")


class BinCapacity(Constraint):
    """Sum of ``max(0, bin load - capacity)``. Default: hard, weight 1."""

    name = "bin_capacity"


class MinimizeBins(Constraint):
    """Number of non-empty bins. Default: soft, weight 1."""

    name = "minimize_bins"


class ColorCompatibility(Constraint):
    """Extra items beyond one item of each color in a bin. Default: hard."""

    name = "color_compatibility"


class ItemConflicts(Constraint):
    """Forbidden item pairs placed in the same bin. Default: hard."""

    name = "item_conflicts"


bin_capacity = BinCapacity()
color_compatibility = ColorCompatibility()
item_conflicts = ItemConflicts()
minimize_bins = MinimizeBins()


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    if constraints is None:
        return [bin_capacity, color_compatibility, item_conflicts, minimize_bins]
    if not constraints:
        raise ValueError("constraints must not be empty (pass None for defaults)")
    return list(constraints)


class Score(NamedTuple):
    hard: int
    soft: float


def _shape(cost: float, exponent: float) -> float:
    if exponent <= 0:
        raise ValueError("constraint exponent must be positive")
    if exponent == 1.0 or cost == 0:
        return cost
    return math.copysign(abs(cost) ** exponent, cost)


def _round_half_away(value: float) -> int:
    return int(math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5))


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    hard = 0
    soft = 0.0
    for spec in get_constraints(constraints):
        entry = costs.get(spec.name)
        if entry is None:
            raise ValueError(
                f"unknown constraint {spec.name!r}; available: " + ", ".join(repr(name) for name in sorted(costs))
            )
        weight = entry["weight"] if spec.weight is None else spec.weight
        is_hard = entry["hard"] if spec.hard is None else spec.hard
        for raw in entry["costs"]:
            value = weight * _shape(raw, spec.exponent)
            if is_hard:
                hard += _round_half_away(value)
            else:
                soft += value
    return Score(-hard, -soft if soft else 0.0)
