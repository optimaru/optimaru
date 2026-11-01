"""Class-based CP-SAT plugins for order picking.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``order_picking.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.
The registry keeps the ``list_constraints()`` order.

Every plugin adds one term per trolley (the engine's unit), empty ones included,
so a pick's ``offset`` / ``clip`` shape each trolley on its own exactly as the
engine does (``terms_per_unit``), and a constraint picked several times adds one
set of terms per pick.
"""

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


class TrolleyCapacityCost(
    domain.TrolleyCapacityCost, ConstraintPlugin[OrderPickingContext]
):
    terms_per_unit = True  # one term per trolley: its buckets short

    def build(self, context: OrderPickingContext, objective: ObjectiveBuilder) -> None:
        bound = sum(len(order.items) for order in context.request.orders)
        for overflow in context.overflow:
            objective.add(self.name, overflow, bound=bound)


class DistanceFromPreviousPickTaskCost(
    domain.DistanceFromPreviousPickTaskCost,
    ConstraintPlugin[OrderPickingContext],
):
    terms_per_unit = True  # one term per trolley: start -> picks, in order

    def build(self, context: OrderPickingContext, objective: ObjectiveBuilder) -> None:
        from order_picking import _core

        tasks = len(context.task_locations)
        longest_step = max(context.between.values(), default=0)
        for trolley, location in enumerate(context.trolley_locations):
            starts = [
                _core.calculate_distance(location, context.task_locations[task])
                for task in range(tasks)
            ]
            walk = [
                distance * context.first[trolley, task]
                for task, distance in enumerate(starts)
            ]
            walk.extend(
                context.between[left, right] * literal
                for (owner, left, right), literal in context.between_arcs.items()
                if owner == trolley
            )
            # The first leg plus at most one step per further pick.
            bound = max(starts, default=0) + max(0, tasks - 1) * longest_step
            objective.add(self.name, sum(walk), bound=bound)


class DistanceFromLastPickTaskToPathOriginCost(
    domain.DistanceFromLastPickTaskToPathOriginCost,
    ConstraintPlugin[OrderPickingContext],
):
    terms_per_unit = True  # one term per trolley: the walk back from its last pick

    def build(self, context: OrderPickingContext, objective: ObjectiveBuilder) -> None:
        from order_picking import _core

        for trolley, location in enumerate(context.trolley_locations):
            backs = [
                _core.calculate_distance(task_location, location)
                for task_location in context.task_locations
            ]
            objective.add(
                self.name,
                sum(
                    distance * context.last[trolley, task]
                    for task, distance in enumerate(backs)
                ),
                bound=max(backs, default=0),
            )


class OrderSplitByTrolleyCost(
    domain.OrderSplitByTrolleyCost, ConstraintPlugin[OrderPickingContext]
):
    terms_per_unit = True  # one term per trolley: the orders it carries

    def build(self, context: OrderPickingContext, objective: ObjectiveBuilder) -> None:
        orders = range(len(context.request.orders))
        for trolley in range(len(context.trolley_locations)):
            objective.add(
                self.name,
                sum(context.order_used[trolley, order] for order in orders),
                bound=len(orders),
            )


REGISTRY = ConstraintRegistry(
    [
        TrolleyCapacityCost(),
        DistanceFromPreviousPickTaskCost(),
        DistanceFromLastPickTaskToPathOriginCost(),
        OrderSplitByTrolleyCost(),
    ]
)
