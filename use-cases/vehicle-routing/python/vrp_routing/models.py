"""Pydantic request/response models for the vehicle-routing solver.

JSON-friendly in/out (snake_case fields, PascalCase classes):

    from vrp_routing.models import (RoutePlanRequest, RoutingInput,
                                    VehicleModel, VisitModel, solve_request)

    request = RoutePlanRequest(
        timelimit=2,
        model_input=RoutingInput(
            vehicles=[VehicleModel(id="V1", capacity=10,
                                   depot=(37.5, 127.0))],
            visits=[VisitModel(id="A", demand=3, location=(37.51, 127.01),
                               min_start_time=8 * 3600, max_end_time=18 * 3600,
                               service_duration=600)],
        ),
    )
    solution = solve_request(request)
    print(solution.routes)          # {"V1": ["A"]}
    print(solution.score)           # "0hard/0medium/-...soft"

Times are integer seconds from a common day start; locations are
(latitude, longitude) pairs. Requires the optional ``pydantic`` dependency.
"""

from typing import Any

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core

__all__ = [
    "RoutePlanRequest",
    "RouteSolution",
    "RoutingInput",
    "VehicleMetrics",
    "VehicleModel",
    "VisitModel",
    "VisitStop",
    "solve_request",
    "to_plan",
    "vehicle_metrics",
    "with_initial_solution",
]


class _Model(BaseModel):
    model_config = ConfigDict(extra="forbid")


class VehicleModel(_Model):
    id: str
    capacity: int
    depot: tuple[float, float]  # (latitude, longitude)
    departure_time: int = 0  # seconds from day start
    max_travel_time: int = Field(default=0, ge=0)
    max_route_duration: int = Field(default=0, ge=0)
    max_end_time: int = Field(default=0, ge=0)
    max_stops: int = Field(default=0, ge=0)
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
            "Visit ids this vehicle's route starts from. The solve keeps them as its "
            "starting point instead of constructing them, and places only what no "
            "route holds."
        ),
    )


class VisitModel(_Model):
    id: str
    location: tuple[float, float]
    demand: int = 1
    min_start_time: int = 0
    max_end_time: int = 24 * 3600
    service_duration: int = 0
    name: str | None = None
    unassigned_penalty_minutes: int = Field(default=-1, ge=-1)


class RoutingInput(_Model):
    vehicles: list[VehicleModel]
    visits: list[VisitModel]
    driving_seconds: list[list[int]] | None = None

    @model_validator(mode="after")
    def _unique_ids(self):
        for kind, items in (("vehicle", self.vehicles), ("visit", self.visits)):
            ids = [x.id for x in items]
            if len(ids) != len(set(ids)):
                raise ValueError(f"duplicate {kind} ids")
        if self.driving_seconds is not None:
            size = len(self.vehicles) + len(self.visits)
            if len(self.driving_seconds) != size or any(
                len(row) != size for row in self.driving_seconds
            ):
                raise ValueError(
                    "driving_seconds must be a square matrix over vehicles then visits"
                )
            if any(value < 0 for row in self.driving_seconds for value in row):
                raise ValueError("driving_seconds values must be non-negative")
        self._check_initial_routes()
        return self

    def _check_initial_routes(self) -> None:
        known = {visit.id for visit in self.visits}
        seen: set[str] = set()
        for vehicle in self.vehicles:
            for visit_id in vehicle.initial_route:
                if visit_id not in known:
                    raise ValueError(f"vehicle {vehicle.id}: unknown visit {visit_id!r}")
                if visit_id in seen:
                    raise ValueError(f"vehicle {vehicle.id}: visit {visit_id!r} appears twice in the initial routes")
                seen.add(visit_id)


class RoutePlanRequest(_Model):
    name: str | None = None
    timelimit: float = Field(default=30.0, ge=0, description="seconds")
    model_input: RoutingInput
    random_seed: int = 0
    num_workers: int = Field(default=1, ge=1)
    stop_at_first_feasible: bool = False
    solver_parameters: dict[str, Any] = Field(default_factory=dict)


class VisitStop(_Model):
    visit_id: str
    arrival_time: int  # seconds from day start
    demand: int


class VehicleMetrics(_Model):
    """Per-vehicle figures derived from a finished route (post-processing).

    Nothing here is scored. The numbers are recomputed from the reported stop
    order and arrival times with the request's driving matrix.
    """

    stops: int = 0
    load: int = 0  # total demand carried
    driving_seconds: int = 0
    departure_time: int
    end_time: int  # departure from the last stop, plus the leg home if it returns
    route_duration_seconds: int = 0  # end_time - departure_time
    waiting_seconds: int = 0  # idle time spent before a visit's window opens
    service_seconds: int = 0
    return_to_depot: bool = False


class RouteSolution(_Model):
    routes: dict[str, list[VisitStop]]  # vehicle id -> ordered stops
    unassigned: list[str] = Field(default_factory=list)  # visit ids
    score: str
    feasible: bool
    total_driving_seconds: int
    vehicle_metrics: dict[str, VehicleMetrics] = Field(default_factory=dict)


def to_plan(model_input: RoutingInput) -> "_core.VehicleRoutePlan":
    """Build the C++ VehicleRoutePlan (locations interned, driving-time
    matrix precomputed) from the pydantic input."""
    locations = [_core.Location(*v.depot) for v in model_input.vehicles]
    vehicles = [
        _core.Vehicle(
            v.id,
            v.capacity,
            i,
            v.departure_time,
            v.max_travel_time,
            v.max_route_duration,
            v.max_end_time,
            v.max_stops,
            v.min_load_ratio,
            v.fixed_cost,
            v.max_route_radius_meters,
            v.return_to_depot,
        )
        for i, v in enumerate(model_input.vehicles)
    ]
    visits = []
    for visit in model_input.visits:
        locations.append(_core.Location(*visit.location))
        visits.append(
            _core.Visit(
                visit.id,
                visit.name or visit.id,
                len(locations) - 1,
                visit.demand,
                visit.min_start_time,
                visit.max_end_time,
                visit.service_duration,
                visit.unassigned_penalty_minutes,
            )
        )
    visit_index = {visit.id: i for i, visit in enumerate(model_input.visits)}
    for vehicle, model in zip(vehicles, model_input.vehicles):
        vehicle.visit_indices = [visit_index[visit_id] for visit_id in model.initial_route]
    # The pool stays empty on purpose. The constraints= path does not clear it,
    # so a visit parked there would be skipped by construction and stay unrouted.
    return _core.VehicleRoutePlan(
        "plan", locations, vehicles, visits, model_input.driving_seconds
    )


def vehicle_metrics(
    plan: "_core.VehicleRoutePlan", routes: dict[str, list[VisitStop]]
) -> dict[str, VehicleMetrics]:
    """Post-process finished routes into per-vehicle figures.

    Works for any backend: it only needs the stop order and the arrival times
    the backend reported, and re-derives departures as
    ``max(arrival, min_start_time) + service_duration``.
    """
    visit_by_id = {visit.id: visit for visit in plan.visits}
    metrics: dict[str, VehicleMetrics] = {}
    for vehicle in plan.vehicles:
        stops = routes.get(vehicle.id, [])
        load = driving = waiting = service = 0
        previous = vehicle.home_location_index
        departure = vehicle.departure_time
        for stop in stops:
            visit = visit_by_id[stop.visit_id]
            load += visit.demand
            driving += plan.driving_time(previous, visit.location_index)
            waiting += max(0, visit.min_start_time - stop.arrival_time)
            service += visit.service_duration
            departure = (
                max(stop.arrival_time, visit.min_start_time) + visit.service_duration
            )
            previous = visit.location_index
        if stops and vehicle.return_to_depot:
            leg = plan.driving_time(previous, vehicle.home_location_index)
            driving += leg
            departure += leg
        metrics[vehicle.id] = VehicleMetrics(
            stops=len(stops),
            load=load,
            driving_seconds=driving,
            departure_time=vehicle.departure_time,
            end_time=departure,
            route_duration_seconds=departure - vehicle.departure_time,
            waiting_seconds=waiting,
            service_seconds=service,
            return_to_depot=vehicle.return_to_depot,
        )
    return metrics


def solve_request(request: RoutePlanRequest, *, constraints=None) -> RouteSolution:
    """Solve a routing request end-to-end and return the pydantic solution."""
    plan = to_plan(request.model_input)

    cfg = _core.SolverConfig()
    cfg.spent_limit_seconds = request.timelimit
    cfg.random_seed = request.random_seed
    cfg.parallel_solver_count = request.num_workers
    _apply_solver_parameters(cfg, request.solver_parameters)
    if request.stop_at_first_feasible:
        cfg.best_score_limit = (0, 0, -(2**62))
    solver = _core.Solver(cfg)
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

    routes: dict[str, list[VisitStop]] = {}
    assigned: set[str] = set()
    for vehicle in plan.vehicles:
        stops = []
        for i in vehicle.visit_indices:
            visit = plan.visits[i]
            stops.append(
                VisitStop(
                    visit_id=visit.id,
                    arrival_time=visit.arrival_time,
                    demand=visit.demand,
                )
            )
            assigned.add(visit.id)
        routes[vehicle.id] = stops
    metrics = vehicle_metrics(plan, routes)

    score = plan.score
    return RouteSolution(
        routes=routes,
        unassigned=[v.id for v in plan.visits if v.id not in assigned],
        score=str(score),
        feasible=score.is_feasible(),
        total_driving_seconds=sum(m.driving_seconds for m in metrics.values()),
        vehicle_metrics=metrics,
    )


def with_initial_solution(problem: RoutePlanRequest, solution: RouteSolution | dict[str, Any]) -> RoutePlanRequest:
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what `solve_request` returned, as the model or as its JSON
    dict. Each vehicle's `initial_route` becomes the visit ids of its route
    there, replacing what `problem` had; a vehicle the solution leaves out
    starts empty. Vehicles and visits are matched by id.

    Visits in no route, `unassigned` ones included, are placed by construction.
    A solution that routes every visit is the exact starting point. Raises
    ValueError on an unknown vehicle or visit, or a visit routed twice.
    """
    routes = RouteSolution.model_validate(solution).routes
    if unknown := routes.keys() - {vehicle.id for vehicle in problem.model_input.vehicles}:
        raise ValueError(f"unknown vehicle {sorted(unknown)}")
    data = problem.model_dump()
    for vehicle in data["model_input"]["vehicles"]:
        vehicle["initial_route"] = [stop.visit_id for stop in routes.get(vehicle["id"], [])]
    return RoutePlanRequest.model_validate(data)


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
