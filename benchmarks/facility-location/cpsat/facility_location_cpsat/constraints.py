"""Class-based CP-SAT plugins for facility location."""

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


class FacilityCapacity(domain.FacilityCapacity, ConstraintPlugin[FacilityContext]):
    default_level = "hard"

    def build(self, context: FacilityContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(context.overflow),
            bound=sum(consumer.demand for consumer in context.problem.consumers),
        )


class FacilitySetupCost(domain.FacilitySetupCost, ConstraintPlugin[FacilityContext]):
    default_level = "soft"
    default_weight = 2

    def build(self, context: FacilityContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(
                facility.setup_cost * context.used[index]
                for index, facility in enumerate(context.problem.facilities)
            ),
            bound=sum(facility.setup_cost for facility in context.problem.facilities),
        )


class DistanceFromFacility(
    domain.DistanceFromFacility, ConstraintPlugin[FacilityContext]
):
    default_level = "soft"
    default_weight = 6

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
    [FacilityCapacity(), FacilitySetupCost(), DistanceFromFacility()]
)
