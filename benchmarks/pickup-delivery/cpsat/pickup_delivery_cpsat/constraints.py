"""Class-based CP-SAT plugins for pickup and delivery routing."""

import math
from dataclasses import dataclass
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model
from pdp_routing import constraints as domain
from pdp_routing.models import PdpInput


@dataclass(frozen=True)
class PdpContext:
    model: cp_model.CpModel
    data: PdpInput
    plan: Any
    nodes: list[Any]
    node_delivery: list[int]
    horizon: int
    assigned: dict[tuple[int, int], Any]
    unassigned: list[Any]
    arrival: list[Any]
    load: dict[tuple[int, int], Any]
    max_capacity: int
    travel_terms: list[Any]
    vehicle_used: dict[int, Any]
    vehicle_travel: dict[int, Any]
    vehicle_end: dict[int, Any]
    vehicle_latency: dict[int, Any]
    vehicle_stop_count: dict[int, Any]
    vehicle_delivered: dict[int, Any]
    vehicle_radius: dict[int, Any]


class VehicleCapacity(domain.VehicleCapacity, ConstraintPlugin[PdpContext]):
    default_level = "hard"

    def build(self, context: PdpContext, objective: ObjectiveBuilder) -> None:
        for vehicle, vehicle_data in enumerate(context.data.vehicles):
            for node in range(len(context.nodes)):
                excess = context.model.new_int_var(
                    0,
                    context.max_capacity,
                    f"vehicle_{vehicle}_node_{node}_capacity_excess",
                )
                context.model.add_max_equality(
                    excess,
                    [0, context.load[vehicle, node] - vehicle_data.capacity],
                )
                objective.add(self.name, excess, bound=context.max_capacity)


class ServiceFinishedAfterMaxEndTime(
    domain.ServiceFinishedAfterMaxEndTime, ConstraintPlugin[PdpContext]
):
    default_level = "hard"

    def build(self, context: PdpContext, objective: ObjectiveBuilder) -> None:
        for node, stop in enumerate(context.nodes):
            late_seconds = context.model.new_int_var(
                0, context.horizon, f"node_{node}_late_seconds"
            )
            context.model.add_max_equality(
                late_seconds,
                [0, context.arrival[node] + stop.service_duration - stop.max_end_time],
            )
            context.model.add(late_seconds == 0).only_enforce_if(
                context.unassigned[context.node_delivery[node]]
            )
            numerator = context.model.new_int_var(
                0, context.horizon + 59, f"node_{node}_late_numerator"
            )
            late_minutes = context.model.new_int_var(
                0, (context.horizon + 59) // 60, f"node_{node}_late_minutes"
            )
            context.model.add(numerator == late_seconds + 59)
            context.model.add_division_equality(late_minutes, numerator, 60)
            objective.add(self.name, late_minutes, bound=(context.horizon + 59) // 60)


class _VehicleLimitPlugin(ConstraintPlugin[PdpContext]):
    limit: str

    def build(self, context: PdpContext, objective: ObjectiveBuilder) -> None:
        for vehicle, vehicle_data in enumerate(context.data.vehicles):
            configured = getattr(vehicle_data, self.limit)
            if configured <= 0:
                continue
            if self.limit == "max_travel_time":
                value, bound = context.vehicle_travel[vehicle], context.horizon
            elif self.limit == "max_route_duration":
                value = context.vehicle_end[vehicle] - vehicle_data.departure_time
                bound = context.horizon
            elif self.limit == "max_end_time":
                value, bound = context.vehicle_end[vehicle], context.horizon
            elif self.limit == "max_stops":
                value, bound = context.vehicle_stop_count[vehicle], len(context.nodes)
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


class UnassignedRequests(domain.UnassignedRequests, ConstraintPlugin[PdpContext]):
    default_level = "medium"

    def build(self, context: PdpContext, objective: ObjectiveBuilder) -> None:
        penalties = [
            (delivery.pickup.service_duration + delivery.dropoff.service_duration) // 60
            for delivery in context.data.deliveries
        ]
        objective.add(
            self.name,
            sum(
                penalties[index] * context.unassigned[index]
                for index in range(len(penalties))
            ),
            bound=sum(penalties),
        )


class MinCapacityUtilization(
    domain.MinCapacityUtilization, ConstraintPlugin[PdpContext]
):
    default_level = "soft"

    def build(self, context: PdpContext, objective: ObjectiveBuilder) -> None:
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
                    - context.vehicle_delivered[vehicle],
                ],
            )
            objective.add(self.name, shortfall, bound=target)


class MinimizeTravelTime(domain.MinimizeTravelTime, ConstraintPlugin[PdpContext]):
    default_level = "soft"

    def build(self, context: PdpContext, objective: ObjectiveBuilder) -> None:
        size = len(context.data.vehicles) + len(context.nodes)
        max_drive = max(
            (
                context.plan.driving_time(left, right)
                for left in range(size)
                for right in range(size)
            ),
            default=0,
        )
        objective.add(self.name, sum(context.travel_terms), bound=max_drive * size)


class MinimizeRouteDuration(domain.MinimizeRouteDuration, ConstraintPlugin[PdpContext]):
    default_level = "soft"
    default_weight = 0

    def build(self, context: PdpContext, objective: ObjectiveBuilder) -> None:
        for vehicle, vehicle_data in enumerate(context.data.vehicles):
            objective.add(
                self.name,
                context.vehicle_end[vehicle] - vehicle_data.departure_time,
                bound=context.horizon,
            )


class MinimizeLatency(domain.MinimizeLatency, ConstraintPlugin[PdpContext]):
    default_level = "soft"
    default_weight = 0

    def build(self, context: PdpContext, objective: ObjectiveBuilder) -> None:
        for latency in context.vehicle_latency.values():
            objective.add(
                self.name, latency, bound=context.horizon * len(context.nodes)
            )


class VehicleFixedCost(domain.VehicleFixedCost, ConstraintPlugin[PdpContext]):
    default_level = "soft"

    def build(self, context: PdpContext, objective: ObjectiveBuilder) -> None:
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
        UnassignedRequests(),
        MinCapacityUtilization(),
        MinimizeTravelTime(),
        MinimizeRouteDuration(),
        MinimizeLatency(),
        VehicleFixedCost(),
    ]
)
