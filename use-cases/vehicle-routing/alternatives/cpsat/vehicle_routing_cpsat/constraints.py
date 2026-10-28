"""Class-based CP-SAT plugins for vehicle routing."""

import math
from dataclasses import dataclass
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model
from vrp_routing import constraints as domain
from vrp_routing.models import RoutingInput


@dataclass(frozen=True)
class RoutingContext:
    model: cp_model.CpModel
    data: RoutingInput
    plan: Any
    horizon: int
    assigned: dict[tuple[int, int], Any]
    unassigned: list[Any]
    arrival: list[Any]
    travel_terms: list[Any]
    vehicle_used: dict[int, Any]
    vehicle_travel: dict[int, Any]
    vehicle_end: dict[int, Any]
    vehicle_latency: dict[int, Any]
    vehicle_stop_count: dict[int, Any]
    vehicle_demand: dict[int, Any]
    vehicle_radius: dict[int, Any]


class VehicleCapacity(domain.VehicleCapacity, ConstraintPlugin[RoutingContext]):
    default_level = "hard"

    def build(self, context: RoutingContext, objective: ObjectiveBuilder) -> None:
        for vehicle, vehicle_data in enumerate(context.data.vehicles):
            bound = sum(max(0, visit.demand) for visit in context.data.visits) + max(
                0, -vehicle_data.capacity
            )
            overflow = context.model.new_int_var(
                0, bound, f"vehicle_{vehicle}_capacity_overflow"
            )
            context.model.add_max_equality(
                overflow,
                [0, context.vehicle_demand[vehicle] - vehicle_data.capacity],
            )
            objective.add(self.name, overflow, bound=bound)


class ServiceFinishedAfterMaxEndTime(
    domain.ServiceFinishedAfterMaxEndTime, ConstraintPlugin[RoutingContext]
):
    default_level = "hard"

    def build(self, context: RoutingContext, objective: ObjectiveBuilder) -> None:
        for visit, visit_data in enumerate(context.data.visits):
            late_seconds = context.model.new_int_var(
                0, context.horizon, f"visit_{visit}_late_seconds"
            )
            context.model.add_max_equality(
                late_seconds,
                [
                    0,
                    context.arrival[visit]
                    + visit_data.service_duration
                    - visit_data.max_end_time,
                ],
            )
            context.model.add(late_seconds == 0).only_enforce_if(
                context.unassigned[visit]
            )
            numerator = context.model.new_int_var(
                0, context.horizon + 59, f"visit_{visit}_late_numerator"
            )
            late_minutes = context.model.new_int_var(
                0,
                (context.horizon + 59) // 60,
                f"visit_{visit}_late_minutes",
            )
            context.model.add(numerator == late_seconds + 59)
            context.model.add_division_equality(late_minutes, numerator, 60)
            objective.add(self.name, late_minutes, bound=(context.horizon + 59) // 60)


class _VehicleLimitPlugin(ConstraintPlugin[RoutingContext]):
    limit: str

    def build(self, context: RoutingContext, objective: ObjectiveBuilder) -> None:
        for vehicle, vehicle_data in enumerate(context.data.vehicles):
            configured = getattr(vehicle_data, self.limit)
            if configured <= 0:
                continue
            if self.limit == "max_travel_time":
                value = context.vehicle_travel[vehicle]
                bound = context.horizon
            elif self.limit == "max_route_duration":
                value = context.vehicle_end[vehicle] - vehicle_data.departure_time
                bound = context.horizon
            elif self.limit == "max_end_time":
                value = context.vehicle_end[vehicle]
                bound = context.horizon
            elif self.limit == "max_stops":
                value = context.vehicle_stop_count[vehicle]
                bound = len(context.data.visits)
            else:
                value = context.vehicle_radius[vehicle]
                configured = math.ceil(configured)
                bound = 40_000_000
            excess = context.model.new_int_var(
                0, bound, f"vehicle_{vehicle}_{self.name}_excess"
            )
            context.model.add_max_equality(excess, [0, value - configured])
            objective.add(self.name, excess, bound=bound)


class MaxTravelTime(domain.MaxTravelTime, _VehicleLimitPlugin):
    default_level = "hard"
    limit = "max_travel_time"


class MaxRouteDuration(domain.MaxRouteDuration, _VehicleLimitPlugin):
    default_level = "hard"
    limit = "max_route_duration"


class VehicleEndTime(domain.VehicleEndTime, _VehicleLimitPlugin):
    default_level = "hard"
    limit = "max_end_time"


class MaxStops(domain.MaxStops, _VehicleLimitPlugin):
    default_level = "hard"
    limit = "max_stops"


class MaxRouteRadius(domain.MaxRouteRadius, _VehicleLimitPlugin):
    default_level = "hard"
    limit = "max_route_radius_meters"


class MaximizeVisitsAssigned(
    domain.MaximizeVisitsAssigned, ConstraintPlugin[RoutingContext]
):
    default_level = "medium"

    def build(self, context: RoutingContext, objective: ObjectiveBuilder) -> None:
        penalties = [visit.service_duration // 60 for visit in context.data.visits]
        objective.add(
            self.name,
            sum(
                penalties[index] * context.unassigned[index]
                for index in range(len(penalties))
            ),
            bound=sum(penalties),
        )


class MinCapacityUtilization(
    domain.MinCapacityUtilization, ConstraintPlugin[RoutingContext]
):
    default_level = "soft"

    def build(self, context: RoutingContext, objective: ObjectiveBuilder) -> None:
        for vehicle, vehicle_data in enumerate(context.data.vehicles):
            if vehicle_data.min_load_ratio <= 0:
                continue
            target = math.ceil(vehicle_data.capacity * vehicle_data.min_load_ratio)
            shortfall = context.model.new_int_var(
                0, target, f"vehicle_{vehicle}_utilization_shortfall"
            )
            context.model.add_max_equality(
                shortfall,
                [
                    0,
                    target * context.vehicle_used[vehicle]
                    - context.vehicle_demand[vehicle],
                ],
            )
            objective.add(self.name, shortfall, bound=target)


class MinimizeTravelTime(domain.MinimizeTravelTime, ConstraintPlugin[RoutingContext]):
    default_level = "soft"

    def build(self, context: RoutingContext, objective: ObjectiveBuilder) -> None:
        size = len(context.data.vehicles) + len(context.data.visits)
        max_drive = max(
            (
                context.plan.driving_time(left, right)
                for left in range(size)
                for right in range(size)
            ),
            default=0,
        )
        objective.add(
            self.name,
            sum(context.travel_terms),
            bound=max_drive * size,
        )


class MinimizeRouteDuration(
    domain.MinimizeRouteDuration, ConstraintPlugin[RoutingContext]
):
    default_level = "soft"
    default_weight = 0

    def build(self, context: RoutingContext, objective: ObjectiveBuilder) -> None:
        for vehicle, vehicle_data in enumerate(context.data.vehicles):
            objective.add(
                self.name,
                context.vehicle_end[vehicle] - vehicle_data.departure_time,
                bound=context.horizon,
            )


class MinimizeLatency(domain.MinimizeLatency, ConstraintPlugin[RoutingContext]):
    default_level = "soft"
    default_weight = 0

    def build(self, context: RoutingContext, objective: ObjectiveBuilder) -> None:
        for latency in context.vehicle_latency.values():
            objective.add(
                self.name,
                latency,
                bound=context.horizon * len(context.data.visits),
            )


class VehicleFixedCost(domain.VehicleFixedCost, ConstraintPlugin[RoutingContext]):
    default_level = "soft"

    def build(self, context: RoutingContext, objective: ObjectiveBuilder) -> None:
        for vehicle, vehicle_data in enumerate(context.data.vehicles):
            cost = max(0, round(vehicle_data.fixed_cost))
            objective.add(self.name, cost * context.vehicle_used[vehicle], bound=cost)


REGISTRY = ConstraintRegistry(
    [
        VehicleCapacity(),
        ServiceFinishedAfterMaxEndTime(),
        MaxTravelTime(),
        MaxRouteDuration(),
        VehicleEndTime(),
        MaxStops(),
        MaxRouteRadius(),
        MaximizeVisitsAssigned(),
        MinCapacityUtilization(),
        MinimizeTravelTime(),
        MinimizeRouteDuration(),
        MinimizeLatency(),
        VehicleFixedCost(),
    ]
)
