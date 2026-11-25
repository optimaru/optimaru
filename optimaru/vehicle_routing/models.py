"""Pydantic request/response models for the vehicle-routing solver.

JSON-friendly in/out (snake_case fields, PascalCase classes):

    from optimaru.vehicle_routing.models import (DepotModel, RoutePlanRequest, RoutingInput,
                                    VehicleModel, VisitModel, solve_request)

    request = RoutePlanRequest(
        time_limit_seconds=2,
        model_input=RoutingInput(
            depots=[DepotModel(id="D", location=(37.5, 127.0))],
            vehicles=[VehicleModel(id="V1", capacity=10, start_depot="D")],
            visits=[VisitModel(id="A", demand=3, location=(37.51, 127.01),
                               min_start_time=8 * 3600, max_end_time=18 * 3600,
                               service_duration=600)],
        ),
    )
    solution = solve_request(request)
    print(solution.routes)          # {"V1": ["A"]}
    print(solution.score)           # "0hard/0medium/-...soft"

Times are integer seconds from a common day start; locations are
(latitude, longitude) pairs. A route starts at its vehicle's `start_depot` and
ends at its `end_depot`, or at its last visit when there is none (an open route).
Driving seconds and meters come from the vehicle's travel profile
(`RoutingInput.profiles`, dicts keyed by depot and visit ids); without one,
straight lines between the coordinates. A profile also names the visits its
vehicles may not serve (`forbidden_visits`) and those they are preferred for
(`preferred_visits`).
"""

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
    "DepotModel",
    "RoutePlanRequest",
    "RouteSolution",
    "RoutingInput",
    "TravelProfile",
    "VehicleMetrics",
    "VehicleModel",
    "VisitModel",
    "VisitStop",
    "parameter_uses",
    "solve_request",
    "to_plan",
    "vehicle_metrics",
    "with_initial_solution",
]


class TravelProfile(_routing.TravelProfile):
    """How a kind of vehicle drives (see `optimaru._routing.TravelProfile`), and which visits it takes."""

    forbidden_visits: list[str] = Field(
        default_factory=list,
        description=(
            "Ids of visits a vehicle on this profile may not serve: each one it serves costs "
            "`profile_forbidden_visit_cost` (hard), which every solve scores, listed or not. A "
            "visit no vehicle may serve stays unassigned."
        ),
    )
    preferred_visits: list[str] = Field(
        default_factory=list,
        description=(
            "Ids of visits that should go to a vehicle on this profile: each one served by a "
            "vehicle whose profile does not prefer it costs `profile_preferred_visit_cost` "
            "(soft). When a vehicle drives on the profile, the constraints of the solve must "
            "include that cost; a profile no vehicle drives on prefers nothing."
        ),
    )

    @model_validator(mode="after")
    def _check_visits(self):
        check_profile_jobs(self, "forbidden_visits", "preferred_visits")
        return self


class VehicleModel(StrictModel):
    id: str
    capacity: int
    start_depot: str = Field(description="Id of the depot the route starts at.")
    end_depot: str | None = Field(
        default=None,
        description=(
            "Id of the depot the route ends at, the start depot or another. None (the "
            "default) is an open route: it ends at its last visit, so no closing leg is "
            "driven or counted toward the route end time."
        ),
    )
    profile: str = Field(
        default=DEFAULT_PROFILE,
        description="Name of the travel profile in `RoutingInput.profiles` the vehicle drives on.",
    )
    departure_time: int = 0  # seconds from day start
    max_travel_time: int = Field(default=0, ge=0)
    max_route_duration: int = Field(default=0, ge=0)
    max_end_time: int = Field(default=0, ge=0)
    max_stops: int = Field(default=0, ge=0)
    min_load_ratio: float = Field(default=0.0, ge=0.0, le=1.0)
    fixed_cost: float = Field(default=0.0, ge=0.0)
    max_route_radius_meters: float = Field(default=0.0, ge=0.0)
    initial_route: list[str] = Field(
        default_factory=list,
        description=(
            "Visit ids this vehicle's route starts from. The solve keeps them as its "
            "starting point instead of constructing them, and places only what no "
            "route holds."
        ),
    )


class VisitModel(StrictModel):
    id: str
    location: tuple[float, float]
    demand: int = 1
    min_start_time: int = 0
    max_end_time: int = 24 * 3600
    service_duration: int = 0
    name: str | None = None
    unassigned_penalty_minutes: int = Field(default=-1, ge=-1)
    group: str | None = Field(
        default=None,
        description=(
            "A name shared by visits that are one job. The visits of a group ride one "
            "vehicle together, or all stay unassigned. None or an empty name is no group."
        ),
    )
    after: list[str] = Field(
        default_factory=list,
        description=(
            "Ids of visits of the same group that are served before this one on the route. "
            "Together these are a partial order on the group: visits it does not relate may "
            "be served in either order, and other visits may come in between."
        ),
    )


class RoutingInput(StrictModel):
    depots: list[DepotModel]
    vehicles: list[VehicleModel]
    visits: list[VisitModel]
    profiles: dict[str, TravelProfile] = Field(
        default_factory=dict,
        description=(
            "Travel profiles by name: driving seconds and meters between depots and visits, "
            "keyed by their ids, and the visits each profile's vehicles may not serve or are "
            "preferred for. Empty (the default): every vehicle drives straight lines between "
            "the coordinates and may serve any visit."
        ),
    )

    @model_validator(mode="after")
    def _unique_ids(self):
        for kind, items in (("vehicle", self.vehicles), ("visit", self.visits)):
            ids = [x.id for x in items]
            if len(ids) != len(set(ids)):
                raise ValueError(f"duplicate {kind} ids")
        check_depots_and_profiles(self.depots, (visit.id for visit in self.visits), self.vehicles, self.profiles)
        known = {visit.id for visit in self.visits}
        for name, profile in self.profiles.items():
            for field in ("forbidden_visits", "preferred_visits"):
                if unknown := sorted(set(getattr(profile, field)) - known):
                    raise ValueError(f"profile {name!r}: {field} names unknown visits {unknown[:10]}")
        self._check_initial_routes()
        self._check_groups()
        return self

    def _check_groups(self) -> None:
        """A visit comes after visits of its own group only, and the order has no cycle."""
        by_id = {visit.id: visit for visit in self.visits}
        for visit in self.visits:
            for earlier in visit.after:
                if earlier not in by_id:
                    raise ValueError(f"visit {visit.id}: `after` names an unknown visit {earlier!r}")
                if earlier == visit.id:
                    raise ValueError(f"visit {visit.id} comes after itself")
                if not visit.group or by_id[earlier].group != visit.group:
                    raise ValueError(f"visit {visit.id} comes after visit {earlier}, which is not in its group")
            if len(set(visit.after)) != len(visit.after):
                raise ValueError(f"visit {visit.id}: `after` names a visit twice")
        # Every visit can be listed behind the ones it comes after, or there is a cycle.
        waiting = {visit.id: len(visit.after) for visit in self.visits}
        followers: dict[str, list[str]] = {}
        for visit in self.visits:
            for earlier in visit.after:
                followers.setdefault(earlier, []).append(visit.id)
        ready = [visit_id for visit_id, count in waiting.items() if count == 0]
        while ready:
            for follower in followers.get(ready.pop(), ()):
                waiting[follower] -= 1
                if waiting[follower] == 0:
                    ready.append(follower)
        if stuck := sorted(visit_id for visit_id, count in waiting.items() if count > 0):
            raise ValueError(f"the order of the visits has a cycle through {stuck}")

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


class RoutePlanRequest(SolveRequest):
    """The routing problem with the settings of one solve (see `SolveRequest` for those)."""

    model_input: RoutingInput


class VisitStop(StrictModel):
    visit_id: str
    arrival_time: int  # seconds from day start
    demand: int


class VehicleMetrics(StrictModel):
    """Per-vehicle figures derived from a finished route (post-processing).

    Nothing here is scored. The numbers are recomputed from the reported stop
    order and arrival times with the vehicle's travel profile.
    """

    stops: int = 0
    load: int = 0  # total demand carried
    driving_seconds: int = 0
    driving_meters: int = 0
    departure_time: int
    end_time: int  # departure from the last stop, plus the leg to the end depot if there is one
    route_duration_seconds: int = 0  # end_time - departure_time
    waiting_seconds: int = 0  # idle time spent before a visit's window opens
    service_seconds: int = 0


class RouteSolution(StrictModel):
    routes: dict[str, list[VisitStop]]  # vehicle id -> ordered stops
    unassigned: list[str] = Field(default_factory=list)  # visit ids
    score: str
    feasible: bool
    total_driving_seconds: int
    total_driving_meters: int = 0
    vehicle_metrics: dict[str, VehicleMetrics] = Field(default_factory=dict)


def to_plan(model_input: RoutingInput) -> "_core.VehicleRoutePlan":
    """Build the C++ VehicleRoutePlan from the pydantic input.

    The locations are the depots, then the visits, in input order; each profile's
    dicts become square matrices over them (`profile_matrices`), and the engine
    fills what a profile does not give from the coordinates. The visit lists of
    the profiles the vehicles drive on become `ProfileVisits` entries, which each
    vehicle names by `profile_visits_index`."""
    locations = [_core.Location(*depot.location) for depot in model_input.depots]
    depot_index = {depot.id: i for i, depot in enumerate(model_input.depots)}
    profile_index, durations, distances = profile_matrices(
        model_input.profiles, [depot.id for depot in model_input.depots] + [visit.id for visit in model_input.visits]
    )
    visit_index = {visit.id: i for i, visit in enumerate(model_input.visits)}
    profile_visits, visits_index_of = profile_job_rules(
        model_input.profiles, model_input.vehicles, visit_index, "forbidden_visits", "preferred_visits"
    )
    vehicles = [
        _core.Vehicle(
            v.id,
            v.capacity,
            depot_index[v.start_depot],
            v.departure_time,
            v.max_travel_time,
            v.max_route_duration,
            v.max_end_time,
            v.max_stops,
            v.min_load_ratio,
            v.fixed_cost,
            v.max_route_radius_meters,
            -1 if v.end_depot is None else depot_index[v.end_depot],
            profile_index[v.profile],
            profile_visits_index,
        )
        for v, profile_visits_index in zip(model_input.vehicles, visits_index_of, strict=True)
    ]
    group_index: dict[str, int] = {}
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
                group_index.setdefault(visit.group, len(group_index)) if visit.group else -1,
                [visit_index[earlier] for earlier in visit.after],
            )
        )
    for vehicle, model in zip(vehicles, model_input.vehicles, strict=True):
        vehicle.visit_indices = [visit_index[visit_id] for visit_id in model.initial_route]
    # The pool stays empty on purpose. The constraints= path does not clear it,
    # so a visit parked there would be skipped by construction and stay unrouted.
    return _core.VehicleRoutePlan(
        "plan",
        locations,
        vehicles,
        visits,
        durations,
        distances,
        [_core.ProfileVisits(forbidden, preferred) for forbidden, preferred in profile_visits],
    )


def vehicle_metrics(plan: "_core.VehicleRoutePlan", routes: dict[str, list[VisitStop]]) -> dict[str, VehicleMetrics]:
    """Post-process finished routes into per-vehicle figures.

    Works for any backend: it only needs the stop order and the arrival times
    the backend reported, and re-derives departures as
    ``max(arrival, min_start_time) + service_duration``.
    """
    visit_by_id = {visit.id: visit for visit in plan.visits}
    metrics: dict[str, VehicleMetrics] = {}
    for vehicle in plan.vehicles:
        stops = routes.get(vehicle.id, [])
        profile = vehicle.profile_index
        load = driving = meters = waiting = service = 0
        previous = vehicle.start_location_index
        departure = vehicle.departure_time
        for stop in stops:
            visit = visit_by_id[stop.visit_id]
            load += visit.demand
            driving += plan.driving_time(previous, visit.location_index, profile)
            meters += plan.driving_distance(previous, visit.location_index, profile)
            waiting += max(0, visit.min_start_time - stop.arrival_time)
            service += visit.service_duration
            departure = max(stop.arrival_time, visit.min_start_time) + visit.service_duration
            previous = visit.location_index
        if stops and vehicle.end_location_index >= 0:
            leg = plan.driving_time(previous, vehicle.end_location_index, profile)
            driving += leg
            meters += plan.driving_distance(previous, vehicle.end_location_index, profile)
            departure += leg
        metrics[vehicle.id] = VehicleMetrics(
            stops=len(stops),
            load=load,
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
    ("max_stops", 0, ("max_stops_cost",)),
    ("min_load_ratio", 0.0, ("min_capacity_utilization_cost",)),
    ("fixed_cost", 0.0, ("vehicle_fixed_cost",)),
    ("max_route_radius_meters", 0.0, ("max_route_radius_cost",)),
)
_VISIT_COSTS = (
    ("max_end_time", 24 * 3600, ("max_end_time_cost",)),
    ("unassigned_penalty_minutes", -1, ("unassigned_visits_cost",)),
    ("group", None, ("visit_group_cost",)),
    ("after", [], ("visit_order_cost",)),
)


def parameter_uses(model_input: RoutingInput) -> list[ParameterUse]:
    """The parameters `model_input` sets away from their defaults that act only through a cost.

    `solve_request` refuses a selection that holds none of a parameter's costs
    (`optimaru._constraint.require_costs`). The table is in the use-case guide
    ("Parameters and their costs"). Forbidden visits are not here (their cost is always on), nor demand and profile
    distances (the solution reports the load and the driving meters from them, whatever the
    constraints)."""
    uses = []
    for vehicle in model_input.vehicles:
        for field, default, costs in _VEHICLE_COSTS:
            if is_set(getattr(vehicle, field), default):
                uses.append(ParameterUse(f"vehicle {vehicle.id}", field, default, costs))
    for visit in model_input.visits:
        for field, default, costs in _VISIT_COSTS:
            if is_set(getattr(visit, field), default):
                uses.append(ParameterUse(f"visit {visit.id}", field, default, costs))
    driven = {vehicle.profile for vehicle in model_input.vehicles}
    for name, profile in model_input.profiles.items():
        # A profile no vehicle drives on prefers nothing, so it asks for no cost.
        if profile.preferred_visits and name in driven:
            uses.append(ParameterUse(f"profile {name!r}", "preferred_visits", [], ("profile_preferred_visit_cost",)))
    return uses


@_api.request_solve("vehicle-routing", "RouteSolution")
def solve_request(request: RoutePlanRequest, *, constraints=None) -> RouteSolution:
    """Solve a routing request end-to-end and return the pydantic solution.

    A parameter set away from its default whose costs the constraints leave out is refused
    with ValueError (`parameter_uses`): the solve would not score it."""
    plan = to_plan(request.model_input)
    constraints = check_parameters(
        parameter_uses(request.model_input),
        constraints,
        get_constraints,
        lambda picks: _core.constraint_costs(plan, picks),
    )

    cfg = _core.SolverConfig()
    cfg.stop.time_limit_seconds = request.time_limit_seconds
    cfg.run.random_seed = request.random_seed
    cfg.run.parallel_solver_count = request.parallel_solver_count
    cfg.run.full_assert = request.full_assert
    apply_solver_parameters(cfg, request.solver_parameters)
    if request.stop_at_first_feasible:
        # Hard 0: -(2**63) on medium/soft is "no floor" (the engine compares hard only).
        cfg.stop.target_score = (0, -(2**63), -(2**63))
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
        # Visit groups and profile visit lists need no entry above: Solver.solve itself scores
        # a plan with either on the default constraints.
        solver.solve(plan)
    else:
        if constraints is None:
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
        total_driving_meters=sum(m.driving_meters for m in metrics.values()),
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
