"""Class-based CP-SAT plugins for bin packing.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``bin_packing.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.
Every plugin's term is the engine's raw cost exactly (0 = no violation).

Every plugin adds one term per engine unit: the three per-bin constraints one
per bin 0..n-1 (an empty bin included), used_bins_cost one for the bin count.
So a pick's ``offset`` / ``clip`` shape each bin (or the count) exactly as the
engine does (``terms_per_unit``), and a constraint picked several times adds
one set of terms per pick. The per-bin overflow and colour-excess variables are
lower bounds that every (monotone) shaped term pushes down to the engine's
value at the optimum.
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
    color_excess: list[list[Any]]  # per bin, per shared colour: max(0, count - 1) at the optimum
    color_bound: int  # the largest possible colour excess of one bin
    conflict_together: list[list[Any]]  # per bin, per listed pair: both items in it


class BinCapacityCost(domain.BinCapacityCost, ConstraintPlugin[BinPackingContext]):
    terms_per_unit = True  # one term per bin

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        bound = sum(item.weight for item in context.problem.items)
        for overflow in context.overflow:
            objective.add(self.name, overflow, bound=bound)


class ColorConflictCost(domain.ColorConflictCost, ConstraintPlugin[BinPackingContext]):
    terms_per_unit = True  # one term per bin: its excess over every shared colour

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        for excess in context.color_excess:
            objective.add(self.name, sum(excess), bound=context.color_bound)


class ItemConflictCost(domain.ItemConflictCost, ConstraintPlugin[BinPackingContext]):
    terms_per_unit = True  # one term per bin: the listed pairs it holds

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        for together in context.conflict_together:
            objective.add(self.name, sum(together), bound=len(together))


class UsedBinsCost(domain.UsedBinsCost, ConstraintPlugin[BinPackingContext]):
    terms_per_unit = True  # a global constraint: one term for the bin count

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(self.name, sum(context.used), bound=len(context.problem.items))


# Plugin order = list_constraints() order.
REGISTRY = ConstraintRegistry([BinCapacityCost(), ColorConflictCost(), ItemConflictCost(), UsedBinsCost()])
