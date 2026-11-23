"""JSON request/response contract shared by PDP solver backends.

A route starts at its vehicle's `start_depot` and ends at its `end_depot`, or at
its last stop when there is none (an open route). Driving seconds and meters
come from the vehicle's travel profile (`PdpInput.profiles`, dicts keyed by depot
and stop ids); without one, straight lines between the coordinates. A profile also
names the requests its vehicles may not carry (`forbidden_requests`) and those they
are preferred for (`preferred_requests`).
"""

from __future__ import annotations

from typing import Any

from pydantic import Field, model_validator

from .. import _api, _routing
from .._constraint import ParameterUse, is_set
from .._models import SolveRequest, StrictModel, apply_solver_parameters
from .._routing import (
    DEFAULT_PROFILE,
    MISSING_TRAVEL_VALUE,
    DepotModel,
    check_depots_and_profiles,
    check_profile_jobs,
    profile_job_rules,
    profile_matrices,
)
from .._solve import check_parameters
from . import _core
from .constraints import get_constraints

__all__ = [
    "DEFAULT_PROFILE",
    "MISSING_TRAVEL_VALUE",
    "DeliveryModel",
    "DepotModel",
    "PdpInput",
    "PdpRequest",
    "PdpSolution",
    "PdpStop",
    "StopModel",
    "TravelProfile",
    "VehicleMetrics",
    "VehicleModel",
    "parameter_uses",
    "solve_request",
    "to_plan",
    "vehicle_metrics",
    "with_initial_solution",
]


class TravelProfile(_routing.TravelProfile):
    """How a kind of vehicle drives (see `optimaru._routing.TravelProfile`), and which requests it carries."""

    forbidden_requests: list[str] = Field(
        default_factory=list,
        description=(
            "Ids of requests a vehicle on this profile may not carry, both stops kept off it: each "
            "stop of one it carries costs `profile_forbidden_request_cost` (hard), which every solve "
            "scores, listed or not. A request no vehicle may carry stays unassigned."
        ),
    )
    preferred_requests: list[str] = Field(
        default_factory=list,
        description=(
            "Ids of requests that should go to a vehicle on this profile: each stop of one carried by "
            "a vehicle whose profile does not prefer it costs `profile_preferred_request_cost` (soft). "
            "When a vehicle drives on the profile, the constraints of the solve must include that "
            "cost; a profile no vehicle drives on prefers nothing."
        ),
    )

    @model_validator(mode="after")
    def _check_requests(self):
        check_profile_jobs(self, "forbidden_requests", "preferred_requests")
        return self


class VehicleModel(StrictModel):
    id: str
    capacity: int = Field(ge=0)
    start_depot: str = Field(description="Id of the depot the route starts at.")
    end_depot: str | None = Field(
        default=None,
        description=(
            "Id of the depot the route ends at, the start depot or another. None (the "
            "default) is an open route: it ends at its last stop, so no closing leg is "
            "driven or counted toward the route end time."
        ),
    )
    profile: str = Field(
        default=DEFAULT_PROFILE,
        description="Name of the travel profile in `PdpInput.profiles` the vehicle drives on.",
    )
    departure_time: int = 0
    max_travel_time: int = Field(default=0, ge=0)
    max_route_duration: int = Field(default=0, ge=0)
    max_end_time: int = Field(default=0, ge=0)
    max_stops: int = Field(
        default=20,
        ge=0,
        description=(
            "Hard cap on the pickup and dropoff stops one vehicle may serve "
            "(a request counts as two). 0 disables the limit; the C++ vehicle and the "
            "exact solver default to 0, this JSON contract defaults to 20."
        ),
    )
    min_load_ratio: float = Field(default=0.0, ge=0.0, le=1.0)
    fixed_cost: float = Field(default=0.0, ge=0.0)
    max_route_radius_meters: float = Field(default=0.0, ge=0.0)
    initial_route: list[str] = Field(
        default_factory=list,
        description=(
            "Stop ids this vehicle's route starts from. The solve keeps them as its "
            "starting point instead of constructing them, and places only what no "
            "route holds. A request's two stops must both be here or both be absent."
        ),
    )
    pinned_stops: int = Field(
        default=0,
        ge=0,
        description=(
            "How many leading stops of `initial_route` cannot change. The rest may "
            "be reordered, moved to another vehicle or have stops inserted between."
        ),
    )


class StopModel(StrictModel):
    id: str
    location: tuple[float, float]
    min_start_time: int = 0
    max_end_time: int = 24 * 3600
    service_duration: int = Field(default=0, ge=0)


class DeliveryModel(StrictModel):
    id: str
    demand: int = Field(ge=0)
    order_time: int = Field(
        default=0,
        description=(
            "When the request was placed, on the plan's clock: negative if it has "
            "already been waiting. Only `squared_order_age_cost` reads it."
        ),
    )
    required_vehicle: str | None = Field(
        default=None,
        description="The vehicle this request must ride on. Its place on the route stays free.",
    )
    group: str | None = Field(
        default=None,
        description=(
            "A name shared by requests that are one job. The requests of a group ride one "
            "vehicle together, or all stay unassigned; their stops may still be visited in "
            "any order, between other stops. None or an empty name is no group."
        ),
    )
    pickup: StopModel
    dropoff: StopModel


class PdpInput(StrictModel):
    depots: list[DepotModel]
    vehicles: list[VehicleModel]
    deliveries: list[DeliveryModel]
    profiles: dict[str, TravelProfile] = Field(
        default_factory=dict,
        description=(
            "Travel profiles by name: driving seconds and meters between depots and stops, "
            "keyed by their ids, and the requests each profile's vehicles may not carry or are "
            "preferred for. Empty (the default): every vehicle drives straight lines between "
            "the coordinates and may carry every request."
        ),
    )

    @model_validator(mode="after")
    def _unique_ids(self):
        vehicle_ids = [vehicle.id for vehicle in self.vehicles]
        delivery_ids = [delivery.id for delivery in self.deliveries]
        stop_ids = [stop.id for delivery in self.deliveries for stop in (delivery.pickup, delivery.dropoff)]
        if len(vehicle_ids) != len(set(vehicle_ids)):
            raise ValueError("duplicate vehicle ids")
        if len(delivery_ids) != len(set(delivery_ids)):
            raise ValueError("duplicate delivery ids")
        if len(stop_ids) != len(set(stop_ids)):
            raise ValueError("duplicate stop ids")
        check_depots_and_profiles(self.depots, stop_ids, self.vehicles, self.profiles)
        self._check_initial_routes()
        for delivery in self.deliveries:
            if delivery.required_vehicle and delivery.required_vehicle not in vehicle_ids:
                raise ValueError(f"request {delivery.id}: unknown vehicle {delivery.required_vehicle!r}")
        self._check_profile_requests()
        self._check_groups()
        return self

    def _check_profile_requests(self) -> None:
        """Known request ids, and no request bound to a vehicle whose profile forbids it."""
        known = {delivery.id for delivery in self.deliveries}
        for name, profile in self.profiles.items():
            for field in ("forbidden_requests", "preferred_requests"):
                if unknown := sorted(set(getattr(profile, field)) - known):
                    raise ValueError(f"profile {name!r}: {field} names unknown requests {unknown[:10]}")
        forbidden = {
            vehicle.id: set(self.profiles[vehicle.profile].forbidden_requests)
            for vehicle in self.vehicles
            if vehicle.profile in self.profiles
        }
        request_of = {stop.id: d.id for d in self.deliveries for stop in (d.pickup, d.dropoff)}
        for delivery in self.deliveries:
            if delivery.required_vehicle and delivery.id in forbidden.get(delivery.required_vehicle, ()):
                raise ValueError(
                    f"request {delivery.id} must ride vehicle {delivery.required_vehicle}, whose profile forbids it"
                )
        for vehicle in self.vehicles:
            for stop_id in vehicle.initial_route[: vehicle.pinned_stops]:
                if request_of[stop_id] in forbidden.get(vehicle.id, ()):
                    raise ValueError(
                        f"vehicle {vehicle.id} pins a stop of request {request_of[stop_id]}, which its profile forbids"
                    )

    def _check_groups(self) -> None:
        """The requests of a group ride one vehicle, so two cannot be bound to different ones."""
        pinned_on = {stop_id: v.id for v in self.vehicles for stop_id in v.initial_route[: v.pinned_stops]}
        bound: dict[str, tuple[str, str]] = {}  # group -> (vehicle, the request that binds it)
        for delivery in self.deliveries:
            if not delivery.group:
                continue
            # A request is bound by its required vehicle, else by the vehicle that pins one of its stops.
            vehicle = (
                delivery.required_vehicle or pinned_on.get(delivery.pickup.id) or pinned_on.get(delivery.dropoff.id)
            )
            if not vehicle:
                continue
            first_vehicle, first = bound.setdefault(delivery.group, (vehicle, delivery.id))
            if first_vehicle != vehicle:
                raise ValueError(
                    f"group {delivery.group!r}: requests {first} and {delivery.id} are bound to different "
                    f"vehicles ({first_vehicle} and {vehicle}) by a required vehicle or a pinned stop"
                )

    def _check_initial_routes(self) -> None:
        requests = {stop.id: delivery for delivery in self.deliveries for stop in (delivery.pickup, delivery.dropoff)}
        seen: set[str] = set()
        for vehicle in self.vehicles:
            route = vehicle.initial_route
            if vehicle.pinned_stops > len(route):
                raise ValueError(f"vehicle {vehicle.id}: more pinned_stops than initial_route stops")
            position = {stop_id: index for index, stop_id in enumerate(route)}
            if len(position) != len(route) or seen.intersection(route):
                raise ValueError(f"vehicle {vehicle.id}: a stop appears twice in the initial routes")
            seen.update(route)
            if unknown := set(route) - requests.keys():
                raise ValueError(f"vehicle {vehicle.id}: unknown stop {sorted(unknown)}")
            for stop_id in route:
                request = requests[stop_id]
                pickup = position.get(request.pickup.id)
                dropoff = position.get(request.dropoff.id)
                if pickup is None or dropoff is None or pickup > dropoff:
                    raise ValueError(
                        f"vehicle {vehicle.id}: request {request.id} needs its pickup, "
                        "then its dropoff, in the same initial route"
                    )


class PdpRequest(SolveRequest):
    """The pickup and delivery problem with the settings of one solve (see `SolveRequest` for those)."""

    model_input: PdpInput


class PdpStop(StrictModel):
    stop_id: str
    delivery_id: str
    pickup: bool
    arrival_time: int


class VehicleMetrics(StrictModel):
    """Per-vehicle figures derived from a finished route (post-processing).

    Nothing here is scored. The numbers are recomputed from the reported stop
    order and arrival times with the vehicle's travel profile, so every
    backend reports them identically for the same routes.
    """

    stops: int = 0
    requests: int = 0  # pickup/dropoff pairs served
    peak_load: int = 0  # most demand carried at any one moment
    delivered: int = 0  # total demand dropped off
    driving_seconds: int = 0
    driving_meters: int = 0
    departure_time: int
    end_time: int  # departure from the last stop, plus the leg to the end depot if there is one
    route_duration_seconds: int = 0  # end_time - departure_time
    waiting_seconds: int = 0  # idle time spent before a stop's window opens
    service_seconds: int = 0


class PdpSolution(StrictModel):
    routes: dict[str, list[PdpStop]]
    unassigned: list[str] = Field(default_factory=list)
    score: str
    feasible: bool
    total_driving_seconds: int
    total_driving_meters: int = 0
    vehicle_metrics: dict[str, VehicleMetrics] = Field(default_factory=dict)


def to_plan(model_input: PdpInput, *, name: str = "plan") -> _core.PdpRoutePlan:
    """Build the C++ PdpRoutePlan from the pydantic input.

    The locations are the depots, then each request's pickup and dropoff, in input
    order; each profile's dicts become square matrices over them
    (`profile_matrices`), and the engine fills what a profile does not give from
    the coordinates. The request lists of the profiles the vehicles drive on become
    `ProfileRequests` entries, which each vehicle names by `profile_requests_index`."""
    locations = [_core.Location(*depot.location) for depot in model_input.depots]
    depot_index = {depot.id: i for i, depot in enumerate(model_input.depots)}
    places = [depot.id for depot in model_input.depots]
    for delivery in model_input.deliveries:
        places += [delivery.pickup.id, delivery.dropoff.id]
    profile_index, durations, distances = profile_matrices(model_input.profiles, places)
    profile_requests, requests_index_of = profile_job_rules(
        model_input.profiles,
        model_input.vehicles,
        {delivery.id: d for d, delivery in enumerate(model_input.deliveries)},
        "forbidden_requests",
        "preferred_requests",
    )
    vehicles = [
        _core.Vehicle(
            vehicle.id,
            vehicle.capacity,
            depot_index[vehicle.start_depot],
            vehicle.departure_time,
            vehicle.max_travel_time,
            vehicle.max_route_duration,
            vehicle.max_end_time,
            vehicle.max_stops,
            vehicle.min_load_ratio,
            vehicle.fixed_cost,
            vehicle.max_route_radius_meters,
            -1 if vehicle.end_depot is None else depot_index[vehicle.end_depot],
            profile_index[vehicle.profile],
            profile_requests_index,
        )
        for vehicle, profile_requests_index in zip(model_input.vehicles, requests_index_of, strict=True)
    ]
    vehicle_index = {vehicle.id: i for i, vehicle in enumerate(model_input.vehicles)}
    group_index: dict[str, int] = {}
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
            _core.Delivery(
                delivery.id,
                pickup_index,
                dropoff_index,
                delivery.demand,
                delivery.order_time,
                vehicle_index[delivery.required_vehicle] if delivery.required_vehicle else -1,
                group_index.setdefault(delivery.group, len(group_index)) if delivery.group else -1,
            )
        )
    node_index = {node.id: index for index, node in enumerate(nodes)}
    for vehicle, model in zip(vehicles, model_input.vehicles):
        route = [node_index[stop_id] for stop_id in model.initial_route]
        vehicle.node_indices = route
        vehicle.pinned_node_indices = route[: model.pinned_stops]
    plan = _core.PdpRoutePlan(
        name,
        locations,
        vehicles,
        nodes,
        deliveries,
        durations,
        distances,
        [_core.ProfileRequests(forbidden, preferred) for forbidden, preferred in profile_requests],
    )
    plan.rebuild_unassigned_pool()
    return plan


def vehicle_metrics(plan: _core.PdpRoutePlan, routes: dict[str, list[PdpStop]]) -> dict[str, VehicleMetrics]:
    """Post-process finished routes into per-vehicle figures.

    Works for any backend: it only needs the stop order and the arrival times
    the backend reported, and re-derives departures as
    ``max(arrival, min_start_time) + service_duration``.
    """
    node_by_id = {node.id: node for node in plan.nodes}
    metrics: dict[str, VehicleMetrics] = {}
    for vehicle in plan.vehicles:
        stops = routes.get(vehicle.id, [])
        profile = vehicle.profile_index
        requests = load = peak = delivered = 0
        driving = meters = waiting = service = 0
        previous = vehicle.start_location_index
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
            driving += plan.driving_time(previous, node.location_index, profile)
            meters += plan.driving_distance(previous, node.location_index, profile)
            waiting += max(0, node.min_start_time - stop.arrival_time)
            service += node.service_duration
            departure = max(stop.arrival_time, node.min_start_time) + node.service_duration
            previous = node.location_index
        if stops and vehicle.end_location_index >= 0:
            leg = plan.driving_time(previous, vehicle.end_location_index, profile)
            driving += leg
            meters += plan.driving_distance(previous, vehicle.end_location_index, profile)
            departure += leg
        metrics[vehicle.id] = VehicleMetrics(
            stops=len(stops),
            requests=requests,
            peak_load=peak,
            delivered=delivered,
            driving_seconds=driving,
            driving_meters=meters,
            departure_time=vehicle.departure_time,
            end_time=departure,
            route_duration_seconds=departure - vehicle.departure_time,
            waiting_seconds=waiting,
            service_seconds=service,
        )
    return metrics


# The parameters that act only through a cost: (model field, default, the costs that read it).
_VEHICLE_COSTS = (
    ("max_travel_time", 0, ("max_travel_time_cost",)),
    ("max_route_duration", 0, ("max_route_duration_cost",)),
    ("max_end_time", 0, ("vehicle_max_end_time_cost",)),
    ("max_stops", 20, ("max_stops_cost",)),
    ("min_load_ratio", 0.0, ("min_capacity_utilization_cost",)),
    ("fixed_cost", 0.0, ("vehicle_fixed_cost",)),
    ("max_route_radius_meters", 0.0, ("max_route_radius_cost",)),
    ("pinned_stops", 0, ("pinned_stops_cost",)),
)
_REQUEST_COSTS = (
    ("order_time", 0, ("squared_order_age_cost",)),
    ("required_vehicle", None, ("required_vehicle_cost",)),
    ("group", None, ("request_group_cost",)),
)


def parameter_uses(model_input: PdpInput) -> list[ParameterUse]:
    """The parameters `model_input` sets away from their defaults that act only through a cost.

    `solve_request` refuses a selection that holds none of a parameter's costs
    (`optimaru._constraint.require_costs`). The table is in the use-case guide
    ("Parameters and their costs"). Forbidden requests are not here (their cost is always on), nor profile distances
    (the solution reports driving meters from them, whatever the constraints)."""
    uses = []
    for vehicle in model_input.vehicles:
        for field, default, costs in _VEHICLE_COSTS:
            value = getattr(vehicle, field)
            # max_stops 0 lifts the cap, so it asks for nothing either.
            if is_set(value, default) and not (field == "max_stops" and value == 0):
                uses.append(ParameterUse(f"vehicle {vehicle.id}", field, default, costs))
    for delivery in model_input.deliveries:
        for field, default, costs in _REQUEST_COSTS:
            if is_set(getattr(delivery, field), default):
                uses.append(ParameterUse(f"request {delivery.id}", field, default, costs))
        for stop in (delivery.pickup, delivery.dropoff):
            if stop.max_end_time != 24 * 3600:
                uses.append(ParameterUse(f"stop {stop.id}", "max_end_time", 24 * 3600, ("max_end_time_cost",)))
    driven = {vehicle.profile for vehicle in model_input.vehicles}
    for name, profile in model_input.profiles.items():
        # A profile no vehicle drives on prefers nothing, so it asks for no cost.
        if profile.preferred_requests and name in driven:
            uses.append(
                ParameterUse(f"profile {name!r}", "preferred_requests", [], ("profile_preferred_request_cost",))
            )
    return uses


@_api.request_solve("pickup-delivery", "PdpSolution")
def solve_request(request: PdpRequest, *, constraints=None) -> PdpSolution:
    """Solve a JSON-friendly request with the metaheuristic backend.

    A parameter set away from its default whose costs the constraints leave out is refused
    with ValueError (`parameter_uses`): the solve would not score it."""
    plan = to_plan(request.model_input, name=request.name or "plan")
    constraints = check_parameters(
        parameter_uses(request.model_input),
        constraints,
        get_constraints,
        lambda picks: _core.constraint_costs(plan, picks),
    )
    config = _core.SolverConfig()
    config.spent_limit_seconds = request.time_limit_seconds
    config.random_seed = request.random_seed
    config.parallel_solver_count = request.parallel_solver_count
    config.full_assert = request.full_assert
    apply_solver_parameters(config, request.solver_parameters)
    if request.stop_at_first_feasible:
        # Hard 0: -(2**63) on medium/soft is "no floor" (the engine compares hard only).
        config.best_score_limit = (0, -(2**63), -(2**63))
    solver = _core.Solver(config)
    has_route_policy = any(
        v.max_travel_time
        or v.max_route_duration
        or v.max_end_time
        or v.max_stops
        or v.min_load_ratio
        or v.fixed_cost
        or v.max_route_radius_meters
        or v.pinned_stops
        for v in request.model_input.vehicles
    ) or any(d.required_vehicle for d in request.model_input.deliveries)
    if constraints is None and not has_route_policy:
        # Request groups and profile request lists need no entry above: Solver.solve itself
        # scores a plan with either on the default constraints.
        solver.solve(plan)
    else:
        if constraints is None:
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
        total_driving_meters=sum(m.driving_meters for m in metrics.values()),
        vehicle_metrics=metrics,
    )


def with_initial_solution(problem: PdpRequest, solution: PdpSolution | dict[str, Any]) -> PdpRequest:
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what `solve_request` returned, as the model or as its JSON
    dict. Each vehicle's `initial_route` becomes the stop ids of its route
    there, replacing what `problem` had; a vehicle the solution leaves out
    starts empty. Vehicles and stops are matched by id.

    Stops in no route, the requests in `unassigned` included, are placed by
    construction. A solution that routes every request is the exact starting
    point.

    A vehicle's `pinned_stops` stay fixed input: its route in `solution` must
    start with them. Raises ValueError otherwise, on an unknown vehicle, and on
    whatever the initial routes refuse: an unknown stop, a stop routed twice, or
    a request whose pickup and dropoff are not together, pickup first. That
    last one means a hard-infeasible result with a broken pair cannot be used
    as a starting point.
    """
    routes = PdpSolution.model_validate(solution).routes
    if unknown := routes.keys() - {vehicle.id for vehicle in problem.model_input.vehicles}:
        raise ValueError(f"unknown vehicle {sorted(unknown)}")
    data = problem.model_dump()
    for vehicle in data["model_input"]["vehicles"]:
        route = [stop.stop_id for stop in routes.get(vehicle["id"], [])]
        pinned = vehicle["initial_route"][: vehicle["pinned_stops"]]
        if route[: len(pinned)] != pinned:
            raise ValueError(f"vehicle {vehicle['id']}: the route does not start with its pinned stops {pinned}")
        vehicle["initial_route"] = route
    return PdpRequest.model_validate(data)
