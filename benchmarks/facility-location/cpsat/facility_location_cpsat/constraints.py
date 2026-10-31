"""Class-based CP-SAT plugins for facility location.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``facility_location.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.
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
    def build(self, context: FacilityContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(context.overflow),
            bound=sum(consumer.demand for consumer in context.problem.consumers),
        )


class FacilitySetupCost(domain.FacilitySetupCost, ConstraintPlugin[FacilityContext]):
    def build(self, context: FacilityContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(
                facility.setup_cost * context.used[index]
                for index, facility in enumerate(context.problem.facilities)
            ),
            bound=sum(facility.setup_cost for facility in context.problem.facilities),
        )


class DistanceFromFacilityCost(
    domain.DistanceFromFacilityCost, ConstraintPlugin[FacilityContext]
):
    def build(self, context: FacilityContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(
                context.distances[consumer][facility] * context.x[consumer, facility]
                for consumer in range(len(context.problem.consumers))
                for facility in range(len(context.problem.facilities))
            ),
            bound=sum(max(row, default=0) for row in context.distances),
        )


REGISTRY = ConstraintRegistry(
    [FacilityCapacityCost(), FacilitySetupCost(), DistanceFromFacilityCost()]
)
