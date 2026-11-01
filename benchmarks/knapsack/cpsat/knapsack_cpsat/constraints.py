"""Class-based CP-SAT plugins for the knapsack use case.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``knapsack.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.
Each one adds exactly the raw cost the engine's component reports — the same
input and selection give the same per-constraint value, and 0 means no
violation (``tests/test_constraint_parity.py``).

Every knapsack constraint is global: each plugin adds its single total in one
``objective.add`` call, the engine's unit, so a pick's ``offset`` / ``clip``
shape that total exactly as the engine does (``terms_per_unit``), and a
constraint picked several times adds one term per pick.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from knapsack import constraints as domain
from knapsack.models import KnapsackProblemModel
from ortools.sat.python import cp_model


@dataclass(frozen=True)
class KnapsackContext:
    model: cp_model.CpModel
    problem: KnapsackProblemModel
    chosen: list[Any]  # one bool var per item: selected
    by_id: dict[str, Any]


class KnapsackCapacityCost(domain.KnapsackCapacityCost, ConstraintPlugin[KnapsackContext]):
    """max(0, selected weight - capacity)."""

    terms_per_unit = True  # one term: the global total, the engine's unit

    def build(self, context: KnapsackContext, objective: ObjectiveBuilder) -> None:
        items = context.problem.items
        total = sum(item.weight for item in items)
        overflow = context.model.new_int_var(0, total, "capacity_overflow")
        load = sum(item.weight * context.chosen[index] for index, item in enumerate(items))
        context.model.add(overflow >= load - context.problem.capacity)
        objective.add(self.name, overflow, bound=total)


class ResourceCapacityCost(domain.ResourceCapacityCost, ConstraintPlugin[KnapsackContext]):
    """Sum over declared resources of max(0, selected usage - capacity)."""

    terms_per_unit = True

    def build(self, context: KnapsackContext, objective: ObjectiveBuilder) -> None:
        items = context.problem.items
        excesses = []
        bound = 0
        for resource, capacity in context.problem.resource_capacities.items():
            usage = [item.resource_usage.get(resource, 0) for item in items]
            excess = context.model.new_int_var(0, sum(usage), f"{resource}_overflow")
            context.model.add(
                excess >= sum(amount * context.chosen[index] for index, amount in enumerate(usage)) - capacity
            )
            excesses.append(excess)
            bound += sum(usage)
        objective.add(self.name, sum(excesses), bound=bound)


class MandatoryItemsCost(domain.MandatoryItemsCost, ConstraintPlugin[KnapsackContext]):
    """One per mandatory entry left out (repeats count)."""

    terms_per_unit = True

    def build(self, context: KnapsackContext, objective: ObjectiveBuilder) -> None:
        entries = context.problem.mandatory_items
        objective.add(self.name, sum(1 - context.by_id[item_id] for item_id in entries), bound=len(entries))


class ForbiddenItemsCost(domain.ForbiddenItemsCost, ConstraintPlugin[KnapsackContext]):
    """One per forbidden entry selected (repeats count)."""

    terms_per_unit = True

    def build(self, context: KnapsackContext, objective: ObjectiveBuilder) -> None:
        entries = context.problem.forbidden_items
        objective.add(self.name, sum(context.by_id[item_id] for item_id in entries), bound=len(entries))


class SelectionGroupBoundsCost(domain.SelectionGroupBoundsCost, ConstraintPlugin[KnapsackContext]):
    """Per group max(0, min - n) + max(0, n - max), n = selected members."""

    terms_per_unit = True

    def build(self, context: KnapsackContext, objective: ObjectiveBuilder) -> None:
        violations = []
        for index, group in enumerate(context.problem.selection_groups):
            count = sum(context.by_id[item_id] for item_id in group.item_ids)
            size = len(group.item_ids)
            maximum = size if group.max_selected is None else group.max_selected
            under = context.model.new_int_var(0, group.min_selected, f"group_{index}_under")
            over = context.model.new_int_var(0, size, f"group_{index}_over")
            context.model.add(under >= group.min_selected - count)
            context.model.add(over >= count - maximum)
            violations.extend((under, over))
        # A group's violation is at most max(min, size - max) <= size.
        bound = sum(len(group.item_ids) for group in context.problem.selection_groups)
        objective.add(self.name, sum(violations), bound=bound)


class ItemDependencyCost(domain.ItemDependencyCost, ConstraintPlugin[KnapsackContext]):
    """One per (item, required) pair with the item selected and the required one not."""

    terms_per_unit = True

    def build(self, context: KnapsackContext, objective: ObjectiveBuilder) -> None:
        violations = []
        for item in context.problem.items:
            for position, required in enumerate(item.requires):
                if required == item.id:
                    continue  # a self-dependency is never violated
                violation = context.model.new_bool_var(f"requires_{item.id}_{position}")
                context.model.add(violation >= context.by_id[item.id] - context.by_id[required])
                violations.append(violation)
        objective.add(self.name, sum(violations), bound=len(violations))


class SelectedValueCost(domain.SelectedValueCost, ConstraintPlugin[KnapsackContext]):
    """The reward: -(selected value)."""

    terms_per_unit = True

    def build(self, context: KnapsackContext, objective: ObjectiveBuilder) -> None:
        items = context.problem.items
        value = sum(item.value * context.chosen[index] for index, item in enumerate(items))
        objective.add_reward(self.name, value, bound=sum(item.value for item in items))


# Plugin order = list_constraints() order.
REGISTRY = ConstraintRegistry(
    [
        KnapsackCapacityCost(),
        ResourceCapacityCost(),
        MandatoryItemsCost(),
        ForbiddenItemsCost(),
        SelectionGroupBoundsCost(),
        ItemDependencyCost(),
        SelectedValueCost(),
    ]
)
