"""Class-based CP-SAT plugins for bin packing.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``bin_packing.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.
Every plugin's term is the engine's raw cost exactly (0 = no violation), summed
over the bins.
"""

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
    # assignments[item, bin]: item in bin (bins 0..n-1, the engine's value range)
    assignments: dict[tuple[int, int], Any]
    used: list[Any]  # per bin: holds at least one item
    overflow: list[Any]  # per bin: max(0, load - capacity) at the optimum
    color_excess: list[Any]  # per (bin, shared colour): max(0, count - 1) at the optimum
    color_bound: int  # the largest possible total colour excess
    conflict_violations: list[Any]  # per listed pair: both items in one bin


class BinCapacityCost(domain.BinCapacityCost, ConstraintPlugin[BinPackingContext]):
    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(context.overflow),
            bound=sum(item.weight for item in context.problem.items),
        )


class ColorConflictCost(domain.ColorConflictCost, ConstraintPlugin[BinPackingContext]):
    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(self.name, sum(context.color_excess), bound=context.color_bound)


class ItemConflictCost(domain.ItemConflictCost, ConstraintPlugin[BinPackingContext]):
    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(context.conflict_violations),
            bound=len(context.problem.conflicts),
        )


class UsedBinsCost(domain.UsedBinsCost, ConstraintPlugin[BinPackingContext]):
    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(self.name, sum(context.used), bound=len(context.problem.items))


# Plugin order = list_constraints() order.
REGISTRY = ConstraintRegistry([BinCapacityCost(), ColorConflictCost(), ItemConflictCost(), UsedBinsCost()])
