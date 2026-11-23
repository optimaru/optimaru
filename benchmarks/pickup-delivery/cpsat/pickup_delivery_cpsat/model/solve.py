"""Pickup/dropoff-coupled CP-SAT circuit formulation.

One circuit per vehicle over its depots (node 0) and the stops (node ``i + 1``: each
request's pickup, then its dropoff). A route starts at the vehicle's ``start_depot``
and is open unless the vehicle has an ``end_depot``, whose leg then ends the route.
Every leg is driven in the vehicle's travel profile (``profile``).
"""

from __future__ import annotations

import itertools
import math
from collections.abc import Mapping, Sequence

from external_solver._plugins import ConstraintConfig
from external_solver.errors import NoSolutionError
from ortools.sat.python import cp_model

from optimaru.pickup_delivery.constraints import Constraint
from optimaru.pickup_delivery.models import (
    PdpRequest,
    PdpSolution,
    PdpStop,
    to_plan,
    vehicle_metrics,
)

from .constraints import REGISTRY, PdpContext

__all__ = ["catalog", "solve_request"]


def _haversine_meters(left: tuple[float, float], right: tuple[float, float]) -> int:
    lat1, lon1 = map(math.radians, left)
    lat2, lon2 = map(math.radians, right)
    dlat, dlon = lat2 - lat1, lon2 - lon1
    value = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    return math.ceil(2 * 6_371_000 * math.asin(math.sqrt(min(1.0, value))))


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def _fix_routes(
    model: cp_model.CpModel,
    route_arcs: dict[int, list[tuple[int, int, object]]],
    vehicle_ids: Sequence[str],
    stop_ids: Sequence[str],
    fixed_routes: Mapping[str, Sequence[str]],
) -> None:
    """Pin every circuit arc to a supplied route (test/evaluation hook)."""
    if set(fixed_routes) != set(vehicle_ids):
        raise ValueError("fixed routes must contain every vehicle exactly once")
    stop_by_id = {stop_id: i + 1 for i, stop_id in enumerate(stop_ids)}
    routed = [stop_id for route in fixed_routes.values() for stop_id in route]
    unknown = set(routed) - set(stop_by_id)
    if unknown:
        raise ValueError(f"unknown fixed-route stops: {sorted(unknown)}")
    if len(routed) != len(set(routed)):
        raise ValueError("a fixed-route stop may appear only once")

    for vehicle_index, vehicle_id in enumerate(vehicle_ids):
        nodes = [stop_by_id[stop_id] for stop_id in fixed_routes[vehicle_id]]
        expected = {(0, 0)} if not nodes else {(0, nodes[0]), (nodes[-1], 0)}
        expected.update(itertools.pairwise(nodes))
        expected.update((node, node) for node in stop_by_id.values() if node not in nodes)
        for tail, head, literal in route_arcs[vehicle_index]:
            model.add(literal == int((tail, head) in expected))


def _longest_legs(plan, stops: Sequence[int]) -> tuple[int, int]:
    """The longest leg any route can drive, in seconds and in meters.

    Only legs a route can take count: a vehicle's start depot to a stop, a stop to another stop,
    and a stop to the vehicle's end depot (none on an open route), each in that vehicle's travel
    profile. Pairs no route drives (depot to depot, a stop back to the start of an open route, a
    profile no vehicle uses) are left out, so the missing value a sparse profile gives them does
    not loosen the horizon and the travel bounds. A drivable leg the profile leaves out still
    counts, so the bound holds for every route.
    """
    drive = meters = 0
    measured: set[int] = set()
    for vehicle in plan.vehicles:
        profile = vehicle.profile_index
        if profile not in measured:
            measured.add(profile)
            for a in stops:
                for b in stops:
                    if a != b:
                        drive = max(drive, plan.driving_time(a, b, profile))
                        meters = max(meters, plan.driving_distance(a, b, profile))
        start, end = vehicle.start_location_index, vehicle.end_location_index
        for stop in stops:
            drive = max(drive, plan.driving_time(start, stop, profile))
            meters = max(meters, plan.driving_distance(start, stop, profile))
            if end >= 0:
                drive = max(drive, plan.driving_time(stop, end, profile))
                meters = max(meters, plan.driving_distance(stop, end, profile))
    return drive, meters


def solve_request(
    request: PdpRequest,
    *,
    constraints: list[ConstraintConfig | Constraint | str | dict] | None = None,
    _fixed_routes: Mapping[str, Sequence[str]] | None = None,
) -> PdpSolution:
    data = request.model_input
    if listed := sorted(
        name for name, profile in data.profiles.items() if profile.forbidden_requests or profile.preferred_requests
    ):
        # Engine-only (profile_forbidden_request_cost, profile_preferred_request_cost): a comparison
        # on such an input would compare two different problems.
        raise ValueError(f"profiles {listed} list forbidden or preferred requests, which this model does not read")
    plan = to_plan(data, name=request.name or "plan")
    model = cp_model.CpModel()
    # The engine's medium folds into soft, so a medium pick may take the literal
    # offset formula; hard refuses it (cpsat-common, in the engine's words).
    objective = REGISTRY.objective(model, ("hard", "medium", "soft"), constraints, medium_fold="into_soft")
    vehicle_count, delivery_count = len(data.vehicles), len(data.deliveries)
    node_count = 2 * delivery_count
    nodes = [stop for delivery in data.deliveries for stop in (delivery.pickup, delivery.dropoff)]
    node_delivery = [d for d in range(delivery_count) for _ in range(2)]
    delta = [value for delivery in data.deliveries for value in (delivery.demand, -delivery.demand)]
    assigned = {
        (v, d): model.new_bool_var(f"vehicle_{v}_delivery_{d}")
        for v in range(vehicle_count)
        for d in range(delivery_count)
    }
    unassigned = [model.new_bool_var(f"delivery_{d}_unassigned") for d in range(delivery_count)]
    for d in range(delivery_count):
        model.add_exactly_one([unassigned[d], *(assigned[v, d] for v in range(vehicle_count))])

    depot_count = len(data.depots)  # the locations are the depots, then the stops
    size = depot_count + node_count
    max_drive, max_meters = _longest_legs(plan, range(depot_count, size))
    legs = node_count + 1  # one leg per stop plus the leg to the end depot
    horizon = (
        max([stop.max_end_time for stop in nodes] + [vehicle.departure_time for vehicle in data.vehicles] + [24 * 3600])
        + sum(stop.service_duration for stop in nodes)
        # A pair a profile leaves out costs its missing value, which may outlast a week.
        + max(7 * 24 * 3600, max_drive * legs)
    )
    arrival = [model.new_int_var(0, horizon, f"node_{i}_arrival") for i in range(node_count)]
    service_start = [model.new_int_var(0, horizon, f"node_{i}_service_start") for i in range(node_count)]
    order = [model.new_int_var(0, max(0, node_count - 1), f"node_{i}_order") for i in range(node_count)]
    for i, stop in enumerate(nodes):
        model.add_max_equality(service_start[i], [arrival[i], stop.min_start_time])

    travel_terms: list = []
    route_arcs: dict[int, list[tuple[int, int, object]]] = {}
    vehicle_used: dict[int, object] = {}
    vehicle_travel: dict[int, object] = {}
    vehicle_distance: dict[int, object] = {}
    vehicle_end: dict[int, object] = {}
    vehicle_latency: dict[int, object] = {}
    vehicle_stop_count: dict[int, object] = {}
    vehicle_delivered: dict[int, object] = {}
    vehicle_radius: dict[int, object] = {}
    max_capacity = max(0, sum(max(0, delivery.demand) for delivery in data.deliveries))
    load = {
        (v, i): model.new_int_var(0, max_capacity, f"vehicle_{v}_node_{i}_load")
        for v in range(vehicle_count)
        for i in range(node_count)
    }
    for v, vehicle in enumerate(data.vehicles):
        compiled = plan.vehicles[v]
        start, end, profile = compiled.start_location_index, compiled.end_location_index, compiled.profile_index
        empty = model.new_bool_var(f"vehicle_{v}_empty")
        vehicle_used[v] = empty.Not()
        count = sum(assigned[v, d] for d in range(delivery_count))
        vehicle_stop_count[v] = 2 * count
        vehicle_delivered[v] = sum(data.deliveries[d].demand * assigned[v, d] for d in range(delivery_count))
        model.add(count == 0).only_enforce_if(empty)
        model.add(count >= 1).only_enforce_if(empty.Not())
        arcs: list[tuple[int, int, object]] = [(0, 0, empty)]
        route_travel_terms: list = []
        distance_terms: list = []
        latency_terms: list = []
        route_end = model.new_int_var(vehicle.departure_time, horizon, f"vehicle_{v}_end")
        model.add(route_end == vehicle.departure_time).only_enforce_if(empty)
        for i in range(node_count):
            present = assigned[v, node_delivery[i]]
            arcs.append((i + 1, i + 1, present.Not()))
            first = model.new_bool_var(f"vehicle_{v}_first_{i}")
            last = model.new_bool_var(f"vehicle_{v}_last_{i}")
            arcs.extend([(0, i + 1, first), (i + 1, 0, last)])
            location = depot_count + i
            from_home = plan.driving_time(start, location, profile)
            # An open route (no end depot, the default) ends at its last stop: no
            # closing leg is driven or added to the route end time.
            to_home = plan.driving_time(location, end, profile) if end >= 0 else 0
            model.add(arrival[i] >= vehicle.departure_time + from_home).only_enforce_if(first)
            model.add(load[v, i] == delta[i]).only_enforce_if(first)
            model.add(order[i] == 0).only_enforce_if(first)
            model.add(route_end == service_start[i] + nodes[i].service_duration + to_home).only_enforce_if(last)
            latency = model.new_int_var(0, horizon, f"vehicle_{v}_node_{i}_latency")
            model.add(latency == arrival[i] - vehicle.departure_time).only_enforce_if(present)
            model.add(latency == 0).only_enforce_if(present.Not())
            latency_terms.append(latency)
            route_travel_terms.extend([from_home * first, to_home * last])
            meters_out = plan.driving_distance(start, location, profile)
            meters_back = plan.driving_distance(location, end, profile) if end >= 0 else 0
            distance_terms.extend([meters_out * first, meters_back * last])
        for i in range(node_count):
            for j in range(node_count):
                if i == j:
                    continue
                arc = model.new_bool_var(f"vehicle_{v}_arc_{i}_{j}")
                arcs.append((i + 1, j + 1, arc))
                drive = plan.driving_time(depot_count + i, depot_count + j, profile)
                model.add(arrival[j] >= service_start[i] + nodes[i].service_duration + drive).only_enforce_if(arc)
                model.add(load[v, j] == load[v, i] + delta[j]).only_enforce_if(arc)
                model.add(order[j] == order[i] + 1).only_enforce_if(arc)
                route_travel_terms.append(drive * arc)
                distance_terms.append(plan.driving_distance(depot_count + i, depot_count + j, profile) * arc)
        model.add_circuit(arcs)
        route_arcs[v] = arcs
        travel_terms.extend(route_travel_terms)
        vehicle_travel[v] = sum(route_travel_terms)
        vehicle_distance[v] = sum(distance_terms)
        vehicle_end[v] = route_end
        vehicle_latency[v] = sum(latency_terms)
        origin = plan.locations[start]  # the engine measures the radius from the start location
        home = (origin.latitude, origin.longitude)
        radius_values = [_haversine_meters(home, stop.location) for stop in nodes]
        radii = [radius_values[i] * assigned[v, node_delivery[i]] for i in range(node_count)]
        furthest = model.new_int_var(0, max(radius_values, default=0), f"vehicle_{v}_route_radius")
        model.add_max_equality(furthest, [0, *radii])
        vehicle_radius[v] = furthest
    if _fixed_routes is not None:
        _fix_routes(
            model,
            route_arcs,
            [vehicle.id for vehicle in data.vehicles],
            [node.id for node in nodes],
            _fixed_routes,
        )

    for d in range(delivery_count):
        pickup, dropoff = 2 * d, 2 * d + 1
        model.add(order[pickup] < order[dropoff]).only_enforce_if(unassigned[d].Not())

    REGISTRY.build(
        PdpContext(
            model,
            data,
            plan,
            nodes,
            node_delivery,
            horizon,
            assigned,
            unassigned,
            arrival,
            load,
            max_capacity,
            travel_terms,
            max_drive * legs,
            max_meters * legs,
            vehicle_used,
            vehicle_travel,
            vehicle_distance,
            vehicle_end,
            vehicle_latency,
            vehicle_stop_count,
            vehicle_delivered,
            vehicle_radius,
        ),
        objective,
    )

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = request.time_limit_seconds
    solver.parameters.random_seed = request.random_seed
    solver.parameters.num_search_workers = request.parallel_solver_count
    status = objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise NoSolutionError(solver.status_name(status))

    routes: dict[str, list[PdpStop]] = {}
    for v, vehicle in enumerate(data.vehicles):
        successors = {
            tail - 1: head - 1 for tail, head, literal in route_arcs[v] if tail != head and solver.value(literal)
        }
        route = []
        current = successors.get(-1, -1)
        while current >= 0:
            d = node_delivery[current]
            route.append(
                PdpStop(
                    stop_id=nodes[current].id,
                    delivery_id=data.deliveries[d].id,
                    pickup=current % 2 == 0,
                    arrival_time=solver.value(arrival[current]),
                )
            )
            current = successors.get(current, -1)
        routes[vehicle.id] = route
    unassigned_ids = [data.deliveries[d].id for d in range(delivery_count) if solver.value(unassigned[d])]
    score = objective.score(solver)
    hard = score["hard"]
    medium = score["medium"]
    # Per-vehicle figures are post-processed from the routes exactly like the
    # C++ backend does, so both report the same numbers for the same routes.
    metrics = vehicle_metrics(plan, routes)
    return PdpSolution(
        routes=routes,
        unassigned=unassigned_ids,
        score=f"{hard}hard/{medium}medium/{score['soft']}soft",
        feasible=hard == 0,
        total_driving_seconds=sum(m.driving_seconds for m in metrics.values()),
        total_driving_meters=sum(m.driving_meters for m in metrics.values()),
        vehicle_metrics=metrics,
    )
