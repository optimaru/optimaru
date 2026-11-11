"""JSON request/response contract shared by PDP solver backends."""

from __future__ import annotations

from typing import Any, TypeVar

from pydantic import Field, model_validator

from .._models import StrictModel, apply_solver_parameters
from .._solve import solver_config
from . import _core
from .constraints import get_constraints

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
    "with_initial_solution",
]


class PdpStop(StrictModel):
    stop_id: str
    delivery_id: str
    pickup: bool
    arrival_time: int


class VehicleMetrics(StrictModel):
    """Per-vehicle figures derived from a finished route (post-processing).

    Nothing here is scored. The numbers are recomputed from the reported stop
    order and arrival times with the request's driving matrix, so every
    backend reports them identically for the same routes.
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


class VehicleModel(StrictModel):
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
            "(a request counts as two). 0 disables the limit; the C++ vehicle and the "
            "exact solver default to 0, this JSON contract defaults to 20."
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
    route: list[PdpStop] | None = Field(
        default=None,
        description=(
            "Result, None on a problem that was not solved: the stops of this vehicle in "
            "order, each with its arrival time. `solve` fills it in; it is not read as input."
        ),
    )
    metrics: VehicleMetrics | None = Field(
        default=None, description="Result: figures derived from `route`. None on a problem that was not solved."
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
    """The pickup and delivery problem, and after `solve` its solution too.

    `solve(problem)` returns a copy with the result fields filled in: each vehicle's
    `route` and `metrics`, `unassigned`, the score levels and `total_driving_seconds`.
    They are None on a problem that was not solved and are never read as input; the
    search starts from `initial_route` (see `with_initial_solution`).
    """

    vehicles: list[VehicleModel]
    deliveries: list[DeliveryModel]
    driving_seconds: list[list[int]] | None = None
    unassigned: list[str] | None = Field(default=None, description="Result: ids of the requests on no route.")
    score_hard: int | None = Field(default=None, description="Result: the hard level of the score. 0 is feasible.")
    score_medium: int | None = Field(default=None, description="Result: the medium level of the score.")
    score_soft: int | None = Field(default=None, description="Result: the soft level of the score.")
    total_driving_seconds: int | None = Field(default=None, description="Result: driving time over all routes.")

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
        if self.driving_seconds is not None:
            size = len(self.vehicles) + 2 * len(self.deliveries)
            if len(self.driving_seconds) != size or any(len(row) != size for row in self.driving_seconds):
                raise ValueError("driving_seconds must be a square matrix over vehicles then stops")
            if any(value < 0 for row in self.driving_seconds for value in row):
                raise ValueError("driving_seconds values must be non-negative")
        self._check_initial_routes()
        for delivery in self.deliveries:
            if delivery.required_vehicle and delivery.required_vehicle not in vehicle_ids:
                raise ValueError(f"request {delivery.id}: unknown vehicle {delivery.required_vehicle!r}")
        self._check_groups()
        return self

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


class PdpRequest(StrictModel):
    name: str | None = None
    timelimit: float = Field(default=30.0, ge=0)
    model_input: PdpInput
    random_seed: int = 0
    num_workers: int = Field(default=1, ge=1)
    stop_at_first_feasible: bool = False
    solver_parameters: dict[str, Any] = Field(default_factory=dict)


class PdpSolution(StrictModel):
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
    plan = _core.PdpRoutePlan(name, locations, vehicles, nodes, deliveries, model_input.driving_seconds)
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
            departure = max(stop.arrival_time, node.min_start_time) + node.service_duration
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
    """Solve a JSON-friendly request with the metaheuristic backend.

    The same as `optimaru.pickup_delivery.solve(request, constraints=constraints)`, which
    also takes the other `solve` keywords. Kept for the callers written before `solve`
    took a request."""
    return _solve_request(request, constraints=constraints)


def _solve_request(
    request: PdpRequest, *, constraints=None, config=None, full_assert: bool = False, on_new_best=None
) -> PdpSolution:
    """`solve(request, ...)`: ``config`` is the base, and the request's time limit, seed and
    worker count replace the base's, as its ``solver_parameters`` replace the fields they name."""
    config = solver_config(
        _core.SolverConfig,
        config,
        time_limit_seconds=request.timelimit,
        random_seed=request.random_seed,
        parallel_solver_count=request.num_workers,
        full_assert=full_assert,
        on_new_best=on_new_best,
    )
    apply_solver_parameters(config, request.solver_parameters)
    if request.stop_at_first_feasible:
        # Hard 0: -(2**63) on medium/soft is "no floor" (the engine compares hard only).
        config.best_score_limit = (0, -(2**63), -(2**63))
    return _solution(_solve_plan(request.model_input, config, constraints, name=request.name or "plan"))


def _solve_model(problem: PdpInput, config: _core.SolverConfig, constraints=None) -> PdpInput:
    """`solve(problem, ...)` for the pydantic problem: a solved copy, `problem` left as it was."""
    plan = _solve_plan(problem, config, constraints)
    solution = _solution(plan)
    vehicles = [
        vehicle.model_copy(
            update={"route": solution.routes[vehicle.id], "metrics": solution.vehicle_metrics[vehicle.id]}
        )
        for vehicle in problem.vehicles
    ]
    return problem.model_copy(
        update={
            "vehicles": vehicles,
            "unassigned": solution.unassigned,
            "score_hard": plan.score.hard,
            "score_medium": plan.score.medium,
            "score_soft": plan.score.soft,
            "total_driving_seconds": solution.total_driving_seconds,
        }
    )


def _solve_plan(
    model_input: PdpInput, config: _core.SolverConfig, constraints=None, *, name: str = "plan"
) -> _core.PdpRoutePlan:
    """Build the plan of `model_input` and solve it: on the core rules, or on the constraint
    assembly when constraints are picked or the input sets a vehicle limit, a pinned stop or a
    required vehicle."""
    plan = to_plan(model_input, name=name)
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
        for v in model_input.vehicles
    ) or any(d.required_vehicle for d in model_input.deliveries)
    if constraints is None and not has_route_policy:
        # Request groups need no entry above: Solver.solve itself scores a plan with a group of
        # two requests or more on the default constraints.
        solver.solve(plan)
    else:
        if constraints is None:
            constraints = get_constraints()
        solver.solve_with_constraints(plan, constraints)

    return plan


def _solution(plan: _core.PdpRoutePlan) -> PdpSolution:
    """The routes of a solved plan, with the figures derived from them."""
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


def _routes_of(solution: PdpInput | PdpSolution | dict[str, Any]) -> dict[str, list[str]]:
    """Stop ids per vehicle id of a result: a solved `PdpInput`, a `PdpSolution`, or the JSON
    dict of either. A vehicle the result does not route is left out."""
    if isinstance(solution, dict):
        solution = (PdpInput if "vehicles" in solution else PdpSolution).model_validate(solution)
    if isinstance(solution, PdpInput):
        return {
            vehicle.id: [stop.stop_id for stop in vehicle.route]
            for vehicle in solution.vehicles
            if vehicle.route is not None
        }
    return {
        vehicle: [stop.stop_id for stop in stops]
        for vehicle, stops in PdpSolution.model_validate(solution).routes.items()
    }


_Problem = TypeVar("_Problem", PdpInput, PdpRequest)


def with_initial_solution(problem: _Problem, solution: PdpInput | PdpSolution | dict[str, Any]) -> _Problem:
    """Return a copy of `problem` whose search starts from `solution`.

    `problem` is a `PdpInput` (or, on the request path, a `PdpRequest`). `solution`
    is what `solve` returned for it, as the model or as its JSON dict: the solved
    `PdpInput`, or the `PdpSolution` of a request. Each vehicle's `initial_route`
    becomes the stop ids of its route there, replacing what `problem` had; a
    vehicle the solution leaves out starts empty. Vehicles and stops are matched
    by id.

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
    routes = _routes_of(solution)
    model_input = problem if isinstance(problem, PdpInput) else problem.model_input
    if unknown := routes.keys() - {vehicle.id for vehicle in model_input.vehicles}:
        raise ValueError(f"unknown vehicle {sorted(unknown)}")
    data = model_input.model_dump()
    for vehicle in data["vehicles"]:
        route = routes.get(vehicle["id"], [])
        pinned = vehicle["initial_route"][: vehicle["pinned_stops"]]
        if route[: len(pinned)] != pinned:
            raise ValueError(f"vehicle {vehicle['id']}: the route does not start with its pinned stops {pinned}")
        vehicle["initial_route"] = route
    seeded = PdpInput.model_validate(data)
    if isinstance(problem, PdpInput):
        return seeded
    return problem.model_copy(update={"model_input": seeded})
