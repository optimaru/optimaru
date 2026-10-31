"""Class-based CP-SAT plugins sharing the canonical domain classes."""

from dataclasses import dataclass
from typing import Any

from bin_packing import constraints as domain
from bin_packing.models import BinPackingProblemModel
from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model


@dataclass(frozen=True)
class BinPackingContext:
    model: cp_model.CpModel
    problem: BinPackingProblemModel
    assignments: dict[tuple[int, int], Any]
    used: list[Any]
    overflow: list[Any]
    color_excess: list[Any]
    conflict_violations: list[Any]


class BinCapacity(domain.BinCapacity, ConstraintPlugin[BinPackingContext]):
    """Sum of each bin's positive load above capacity."""

    default_level = "hard"
    default_weight = 1
    label = "Bin capacity"

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(context.overflow),
            bound=sum(item.weight for item in context.problem.items),
        )


class MinimizeBins(domain.MinimizeBins, ConstraintPlugin[BinPackingContext]):
    """Number of non-empty bins in the solution."""

    default_level = "soft"
    default_weight = 1
    label = "Minimize used bins"

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(self.name, sum(context.used), bound=len(context.problem.items))


class ColorCompatibility(
    domain.ColorCompatibility, ConstraintPlugin[BinPackingContext]
):
    """Extra same-color items assigned to one bin."""

    default_level = "hard"
    default_weight = 1
    label = "Color compatibility"

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name, sum(context.color_excess), bound=len(context.problem.items)
        )


class ItemConflicts(domain.ItemConflicts, ConstraintPlugin[BinPackingContext]):
    """Forbidden item pairs assigned to one bin."""

    default_level = "hard"
    default_weight = 1
    label = "Item conflicts"

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(context.conflict_violations),
            bound=len(context.problem.conflicts),
        )


REGISTRY = ConstraintRegistry(
    [BinCapacity(), ColorCompatibility(), ItemConflicts(), MinimizeBins()]
)
