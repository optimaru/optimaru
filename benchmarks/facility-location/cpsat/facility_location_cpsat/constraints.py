"""Class-based CP-SAT plugins for facility location.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``facility_location.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.

Every plugin adds one term per facility (the engine's unit), unused ones
included, so a pick's ``offset`` / ``clip`` shape each facility on its own
exactly as the engine does (``terms_per_unit``), and a constraint picked
several times adds one set of terms per pick.
"""

from dataclasses import dataclass
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from facility_location import constraints as domain
from facility_location.models import FacilityLocationProblemModel
from ortools.sat.python import cp_model


@dataclass(frozen=True)
class FacilityContext:
    model: cp_model.CpModel
    problem: FacilityLocationProblemModel
    x: dict[tuple[int, int], Any]
    used: list[Any]
    overflow: list[Any]
    distances: list[list[int]]


class FacilityCapacityCost(domain.FacilityCapacityCost, ConstraintPlugin[FacilityContext]):
    terms_per_unit = True  # one term per facility, like the engine's units

    def build(self, context: FacilityContext, objective: ObjectiveBuilder) -> None:
        total_demand = sum(consumer.demand for consumer in context.problem.consumers)
        for overflow in context.overflow:
            objective.add(self.name, overflow, bound=total_demand)


class FacilitySetupCost(domain.FacilitySetupCost, ConstraintPlugin[FacilityContext]):
    terms_per_unit = True

    def build(self, context: FacilityContext, objective: ObjectiveBuilder) -> None:
        for index, facility in enumerate(context.problem.facilities):
            objective.add(
                self.name,
                facility.setup_cost * context.used[index],
                bound=facility.setup_cost,
            )


class DistanceFromFacilityCost(
    domain.DistanceFromFacilityCost, ConstraintPlugin[FacilityContext]
):
    terms_per_unit = True

    def build(self, context: FacilityContext, objective: ObjectiveBuilder) -> None:
        consumers = range(len(context.problem.consumers))
        for facility in range(len(context.problem.facilities)):
            objective.add(
                self.name,
                sum(
                    context.distances[consumer][facility] * context.x[consumer, facility]
                    for consumer in consumers
                ),
                bound=sum(context.distances[consumer][facility] for consumer in consumers),
            )


REGISTRY = ConstraintRegistry(
    [FacilityCapacityCost(), FacilitySetupCost(), DistanceFromFacilityCost()]
)
