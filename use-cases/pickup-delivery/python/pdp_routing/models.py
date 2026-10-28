"""JSON request/response contract shared by PDP solver backends."""

from __future__ import annotations

from typing import Any

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core

__all__ = [
    "DeliveryModel",
    "PdpInput",
    "PdpRequest",
    "PdpSolution",
    "PdpStop",
    "StopModel",
    "VehicleMetrics",
    "VehicleModel",
    "solve_request",
    "to_plan",
    "vehicle_metrics",
]


class _Model(BaseModel):
    model_config = ConfigDict(extra="forbid")


class VehicleModel(_Model):
    id: str
    capacity: int = Field(ge=0)
    home: tuple[float, float]
    departure_time: int = 0
    max_travel_time: int = Field(default=0, ge=0)
    max_route_duration: int = Field(default=0, ge=0)
    max_end_time: int = Field(default=0, ge=0)
    max_stops: int = Field(
        default=20,
        ge=0,
        description=(
            "Hard cap on the pickup and dropoff stops one vehicle may serve "
            "(a request counts as two). 0 disables the limit; the C++ and CP-SAT "
            "vehicles default to 0, this JSON contract defaults to 20."
        ),
    )
    min_load_ratio: float = Field(default=0.0, ge=0.0, le=1.0)
    fixed_cost: float = Field(default=0.0, ge=0.0)
    max_route_radius_meters: float = Field(default=0.0, ge=0.0)
    return_to_depot: bool = Field(
        default=False,
        description=(
            "Whether the route ends back at the vehicle's start. Off by default: "
            "the route is open and ends at its last stop, so no closing leg is "
            "driven or counted toward the route end time."
        ),
    )


class StopModel(_Model):
    id: str
    location: tuple[float, float]
    min_start_time: int = 0
    max_end_time: int = 24 * 3600
    service_duration: int = Field(default=0, ge=0)


class DeliveryModel(_Model):
    id: str
    demand: int = Field(ge=0)
    pickup: StopModel
    dropoff: StopModel


class PdpInput(_Model):
    vehicles: list[VehicleModel]
    deliveries: list[DeliveryModel]
    driving_seconds: list[list[int]] | None = None

    @model_validator(mode="after")
    def _unique_ids(self):
        vehicle_ids = [vehicle.id for vehicle in self.vehicles]
        delivery_ids = [delivery.id for delivery in self.deliveries]
        stop_ids = [
            stop.id
            for delivery in self.deliveries
            for stop in (delivery.pickup, delivery.dropoff)
        ]
        if len(vehicle_ids) != len(set(vehicle_ids)):
            raise ValueError("duplicate vehicle ids")
        if len(delivery_ids) != len(set(delivery_ids)):
            raise ValueError("duplicate delivery ids")
        if len(stop_ids) != len(set(stop_ids)):
            raise ValueError("duplicate stop ids")
        if self.driving_seconds is not None:
            size = len(self.vehicles) + 2 * len(self.deliveries)
            if len(self.driving_seconds) != size or any(
                len(row) != size for row in self.driving_seconds
            ):
                raise ValueError(
                    "driving_seconds must be a square matrix over vehicles then stops"
                )
            if any(value < 0 for row in self.driving_seconds for value in row):
                raise ValueError("driving_seconds values must be non-negative")
        return self


class PdpRequest(_Model):
    name: str | None = None
    timelimit: float = Field(default=30.0, ge=0)
    model_input: PdpInput
    random_seed: int = 0
    num_workers: int = Field(default=1, ge=1)
    stop_at_first_feasible: bool = False
    solver_parameters: dict[str, Any] = Field(default_factory=dict)


class PdpStop(_Model):
    stop_id: str
    delivery_id: str
    pickup: bool
    arrival_time: int


class VehicleMetrics(_Model):
    """Per-vehicle figures derived from a finished route (post-processing).

    Nothing here is scored. The numbers are recomputed from the reported stop
    order and arrival times with the request's driving matrix, so the C++ and
    CP-SAT backends report them identically for the same routes.
    """

    stops: int = 0
    requests: int = 0  # pickup/dropoff pairs served
    peak_load: int = 0  # most demand carried at any one moment
    delivered: int = 0  # total demand dropped off
    driving_seconds: int = 0
    departure_time: int
    end_time: int  # departure from the last stop, plus the leg home if it returns
    route_duration_seconds: int = 0  # end_time - departure_time
    waiting_seconds: int = 0  # idle time spent before a stop's window opens
    service_seconds: int = 0
    return_to_depot: bool = False


class PdpSolution(_Model):
    routes: dict[str, list[PdpStop]]
    unassigned: list[str] = Field(default_factory=list)
    score: str
    feasible: bool
    total_driving_seconds: int
    vehicle_metrics: dict[str, VehicleMetrics] = Field(default_factory=dict)


def to_plan(model_input: PdpInput, *, name: str = "plan") -> _core.PdpRoutePlan:
    locations = [_core.Location(*vehicle.home) for vehicle in model_input.vehicles]
    vehicles = [
        _core.Vehicle(
            vehicle.id,
            vehicle.capacity,
            i,
            vehicle.departure_time,
            vehicle.max_travel_time,
            vehicle.max_route_duration,
            vehicle.max_end_time,
            vehicle.max_stops,
            vehicle.min_load_ratio,
            vehicle.fixed_cost,
            vehicle.max_route_radius_meters,
            vehicle.return_to_depot,
        )
        for i, vehicle in enumerate(model_input.vehicles)
    ]
    nodes = []
    deliveries = []
    for d, delivery in enumerate(model_input.deliveries):
        pickup_index = len(nodes)
        locations.append(_core.Location(*delivery.pickup.location))
        nodes.append(
            _core.Node(
                delivery.pickup.id,
                len(locations) - 1,
                delivery.pickup.min_start_time,
                delivery.pickup.max_end_time,
                delivery.pickup.service_duration,
                d,
                True,
            )
        )
        dropoff_index = len(nodes)
        locations.append(_core.Location(*delivery.dropoff.location))
        nodes.append(
            _core.Node(
                delivery.dropoff.id,
                len(locations) - 1,
                delivery.dropoff.min_start_time,
                delivery.dropoff.max_end_time,
                delivery.dropoff.service_duration,
                d,
                False,
            )
        )
        deliveries.append(
            _core.Delivery(delivery.id, pickup_index, dropoff_index, delivery.demand)
        )
    return _core.PdpRoutePlan(
        name, locations, vehicles, nodes, deliveries, model_input.driving_seconds
    )


def vehicle_metrics(
    plan: _core.PdpRoutePlan, routes: dict[str, list[PdpStop]]
) -> dict[str, VehicleMetrics]:
    """Post-process finished routes into per-vehicle figures.

    Works for any backend: it only needs the stop order and the arrival times
    the backend reported, and re-derives departures as
    ``max(arrival, min_start_time) + service_duration``.
    """
    node_by_id = {node.id: node for node in plan.nodes}
    metrics: dict[str, VehicleMetrics] = {}
    for vehicle in plan.vehicles:
        stops = routes.get(vehicle.id, [])
        requests = load = peak = delivered = 0
        driving = waiting = service = 0
        previous = vehicle.home_location_index
        departure = vehicle.departure_time
        for stop in stops:
            node = node_by_id[stop.stop_id]
            demand = plan.deliveries[node.delivery_index].demand
            if node.pickup:
                requests += 1
                load += demand
                peak = max(peak, load)
            else:
                load -= demand
                delivered += demand
            driving += plan.driving_time(previous, node.location_index)
            waiting += max(0, node.min_start_time - stop.arrival_time)
            service += node.service_duration
            departure = (
                max(stop.arrival_time, node.min_start_time) + node.service_duration
            )
            previous = node.location_index
        if stops and vehicle.return_to_depot:
            leg = plan.driving_time(previous, vehicle.home_location_index)
            driving += leg
            departure += leg
        metrics[vehicle.id] = VehicleMetrics(
            stops=len(stops),
            requests=requests,
            peak_load=peak,
            delivered=delivered,
            driving_seconds=driving,
            departure_time=vehicle.departure_time,
            end_time=departure,
            route_duration_seconds=departure - vehicle.departure_time,
            waiting_seconds=waiting,
            service_seconds=service,
            return_to_depot=vehicle.return_to_depot,
        )
    return metrics


def solve_request(request: PdpRequest, *, constraints=None) -> PdpSolution:
    """Solve a JSON-friendly request with the metaheuristic backend."""
    plan = to_plan(request.model_input, name=request.name or "plan")
    config = _core.SolverConfig()
    config.spent_limit_seconds = request.timelimit
    config.random_seed = request.random_seed
    config.parallel_solver_count = request.num_workers
    _apply_solver_parameters(config, request.solver_parameters)
    if request.stop_at_first_feasible:
        config.best_score_limit = (0, 0, -(2**62))
    solver = _core.Solver(config)
    has_route_policy = any(
        v.max_travel_time
        or v.max_route_duration
        or v.max_end_time
        or v.max_stops
        or v.min_load_ratio
        or v.fixed_cost
        or v.max_route_radius_meters
        for v in request.model_input.vehicles
    )
    if constraints is None and not has_route_policy:
        solver.solve(plan)
    else:
        if constraints is None:
            from .constraints import get_constraints

            constraints = get_constraints()
        solver.solve_with_constraints(plan, constraints)

    routes: dict[str, list[PdpStop]] = {}
    for vehicle in plan.vehicles:
        stops = []
        for node_index in vehicle.node_indices:
            node = plan.nodes[node_index]
            delivery = plan.deliveries[node.delivery_index]
            stops.append(
                PdpStop(
                    stop_id=node.id,
                    delivery_id=delivery.id,
                    pickup=node.pickup,
                    arrival_time=node.arrival_time,
                )
            )
        routes[vehicle.id] = stops
    metrics = vehicle_metrics(plan, routes)
    return PdpSolution(
        routes=routes,
        unassigned=plan.unassigned_delivery_ids(),
        score=str(plan.score),
        feasible=plan.score.is_feasible(),
        total_driving_seconds=sum(m.driving_seconds for m in metrics.values()),
        vehicle_metrics=metrics,
    )


def _apply_solver_parameters(config, parameters: dict[str, Any]) -> None:
    for name, value in parameters.items():
        if name in {
            "spent_limit_seconds",
            "random_seed",
            "parallel_solver_count",
            "on_new_best",
        }:
            raise ValueError(
                f"solver parameter {name!r} is controlled by the solve request"
            )
        if (
            name.startswith("_")
            or not hasattr(config, name)
            or callable(getattr(config, name))
        ):
            raise ValueError(f"unknown solver parameter: {name}")
        if value is None:
            continue
        if name == "best_score_limit" and isinstance(value, list):
            value = tuple(value)
        current = getattr(config, name)
        if isinstance(value, str) and hasattr(type(current), value):
            value = getattr(type(current), value)
        try:
            setattr(config, name, value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"invalid solver parameter {name!r}: {exc}") from exc
