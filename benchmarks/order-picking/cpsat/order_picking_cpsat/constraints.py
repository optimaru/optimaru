"""Class-based CP-SAT plugins for order picking."""

from dataclasses import dataclass
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from order_picking import constraints as domain
from order_picking.models import OrderPickingRequest
from ortools.sat.python import cp_model


@dataclass(frozen=True)
class OrderPickingContext:
    model: cp_model.CpModel
    request: OrderPickingRequest
    task_locations: list[Any]
    trolley_locations: list[Any]
    first: dict[tuple[int, int], Any]
    last: dict[tuple[int, int], Any]
    between_arcs: dict[tuple[int, int, int], Any]
    between: dict[tuple[int, int], int]
    order_used: dict[tuple[int, int], Any]
    overflow: list[Any]


class RequiredNumberOfBuckets(
    domain.RequiredNumberOfBuckets, ConstraintPlugin[OrderPickingContext]
):
    default_level = "hard"

    def build(self, context: OrderPickingContext, objective: ObjectiveBuilder) -> None:
        bound = sum(len(order.items) for order in context.request.orders)
        for overflow in context.overflow:
            objective.add(self.name, overflow, bound=bound)


class MinimizeDistanceFromPreviousPickTask(
    domain.MinimizeDistanceFromPreviousPickTask,
    ConstraintPlugin[OrderPickingContext],
):
    default_level = "soft"

    def build(self, context: OrderPickingContext, objective: ObjectiveBuilder) -> None:
        from order_picking import _core

        for (trolley, task), literal in context.first.items():
            distance = _core.calculate_distance(
                context.trolley_locations[trolley], context.task_locations[task]
            )
            objective.add(self.name, distance * literal, bound=distance)
        for (_trolley, left, right), literal in context.between_arcs.items():
            distance = context.between[left, right]
            objective.add(self.name, distance * literal, bound=distance)


class MinimizeDistanceFromLastPickTaskToPathOrigin(
    domain.MinimizeDistanceFromLastPickTaskToPathOrigin,
    ConstraintPlugin[OrderPickingContext],
):
    default_level = "soft"

    def build(self, context: OrderPickingContext, objective: ObjectiveBuilder) -> None:
        from order_picking import _core

        for (trolley, task), literal in context.last.items():
            distance = _core.calculate_distance(
                context.task_locations[task], context.trolley_locations[trolley]
            )
            objective.add(self.name, distance * literal, bound=distance)


class MinimizeOrderSplitByTrolley(
    domain.MinimizeOrderSplitByTrolley, ConstraintPlugin[OrderPickingContext]
):
    default_level = "soft"
    default_weight = 1_000

    def build(self, context: OrderPickingContext, objective: ObjectiveBuilder) -> None:
        for used in context.order_used.values():
            objective.add(self.name, used, bound=1)


REGISTRY = ConstraintRegistry(
    [
        RequiredNumberOfBuckets(),
        MinimizeDistanceFromPreviousPickTask(),
        MinimizeDistanceFromLastPickTaskToPathOrigin(),
        MinimizeOrderSplitByTrolley(),
    ]
)
