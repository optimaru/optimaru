"""Multiple-circuit CP-SAT formulation for vehicle routing."""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence

from ortools.sat.python import cp_model

from cpsat_plugins import ConstraintConfig
from vrp_routing.constraints import Constraint
from vrp_routing.models import (
    RoutePlanRequest,
    RouteSolution,
    VisitStop,
    to_plan,
    vehicle_metrics,
)

from .constraints import REGISTRY, RoutingContext

__all__ = ["catalog", "solve_request"]


def _haversine_meters(left: tuple[float, float], right: tuple[float, float]) -> int:
    lat1, lon1 = map(math.radians, left)
    lat2, lon2 = map(math.radians, right)
    dlat, dlon = lat2 - lat1, lon2 - lon1
    value = (
        math.sin(dlat / 2) ** 2
        + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    )
    return math.ceil(2 * 6_371_000 * math.asin(math.sqrt(min(1.0, value))))


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def _fix_routes(
    model: cp_model.CpModel,
    route_arcs: dict[int, list[tuple[int, int, object]]],
    vehicle_ids: Sequence[str],
    visit_ids: Sequence[str],
    fixed_routes: Mapping[str, Sequence[str]],
) -> None:
    """Pin every circuit arc to a supplied route (test/evaluation hook)."""
    if set(fixed_routes) != set(vehicle_ids):
        raise ValueError("fixed routes must contain every vehicle exactly once")
    visit_by_id = {visit_id: i + 1 for i, visit_id in enumerate(visit_ids)}
    routed = [visit_id for route in fixed_routes.values() for visit_id in route]
    unknown = set(routed) - set(visit_by_id)
    if unknown:
        raise ValueError(f"unknown fixed-route visits: {sorted(unknown)}")
    if len(routed) != len(set(routed)):
        raise ValueError("a fixed-route visit may appear only once")

    for vehicle_index, vehicle_id in enumerate(vehicle_ids):
        nodes = [visit_by_id[visit_id] for visit_id in fixed_routes[vehicle_id]]
        expected = {(0, 0)} if not nodes else {(0, nodes[0]), (nodes[-1], 0)}
        expected.update(zip(nodes, nodes[1:]))
        expected.update(
            (node, node) for node in visit_by_id.values() if node not in nodes
        )
        for tail, head, literal in route_arcs[vehicle_index]:
            model.add(literal == int((tail, head) in expected))


def solve_request(
    request: RoutePlanRequest,
    *,
    constraints: list[ConstraintConfig | Constraint | str | dict] | None = None,
    _fixed_routes: Mapping[str, Sequence[str]] | None = None,
) -> RouteSolution:
    """Solve using the same request/response contract as the C++ backend."""
    data = request.model_input
    plan = to_plan(data)
    model = cp_model.CpModel()
    objective = REGISTRY.objective(model, ("hard", "medium", "soft"), constraints)
    vehicle_count, visit_count = len(data.vehicles), len(data.visits)
    horizon = (
        max(
            [visit.max_end_time for visit in data.visits]
            + [vehicle.departure_time for vehicle in data.vehicles]
            + [24 * 3600]
        )
        + sum(visit.service_duration for visit in data.visits)
        + 7 * 24 * 3600
    )
    assigned = {
        (v, i): model.new_bool_var(f"vehicle_{v}_visit_{i}")
        for v in range(vehicle_count)
        for i in range(visit_count)
    }
    unassigned = [
        model.new_bool_var(f"visit_{i}_unassigned") for i in range(visit_count)
    ]
    arrival = [
        model.new_int_var(0, horizon, f"visit_{i}_arrival") for i in range(visit_count)
    ]
    service_start = [
        model.new_int_var(0, horizon, f"visit_{i}_service_start")
        for i in range(visit_count)
    ]
    for i, visit in enumerate(data.visits):
        model.add_exactly_one(
            [unassigned[i], *(assigned[v, i] for v in range(vehicle_count))]
        )
        model.add_max_equality(service_start[i], [arrival[i], visit.min_start_time])

    travel_terms: list = []
    route_arcs: dict[int, list[tuple[int, int, object]]] = {}
    vehicle_used: dict[int, object] = {}
    vehicle_travel: dict[int, object] = {}
    vehicle_end: dict[int, object] = {}
    vehicle_latency: dict[int, object] = {}
    vehicle_stop_count: dict[int, object] = {}
    vehicle_demand: dict[int, object] = {}
    vehicle_radius: dict[int, object] = {}
    for v, vehicle in enumerate(data.vehicles):
        empty = model.new_bool_var(f"vehicle_{v}_empty")
        vehicle_used[v] = empty.Not()
        selected_count = sum(assigned[v, i] for i in range(visit_count))
        vehicle_stop_count[v] = selected_count
        model.add(selected_count == 0).only_enforce_if(empty)
        model.add(selected_count >= 1).only_enforce_if(empty.Not())
        arcs: list[tuple[int, int, object]] = [(0, 0, empty)]
        route_travel_terms: list = []
        latency_terms: list = []
        route_end = model.new_int_var(
            vehicle.departure_time, horizon, f"vehicle_{v}_end"
        )
        model.add(route_end == vehicle.departure_time).only_enforce_if(empty)
        for i in range(visit_count):
            node = i + 1
            arcs.append((node, node, assigned[v, i].Not()))
            first = model.new_bool_var(f"vehicle_{v}_first_{i}")
            last = model.new_bool_var(f"vehicle_{v}_last_{i}")
            arcs.extend([(0, node, first), (node, 0, last)])
            from_home = plan.driving_time(v, vehicle_count + i)
            # An open route (the default) ends at its last visit: the leg back
            # home is neither driven nor added to the route end time.
            to_home = (
                plan.driving_time(vehicle_count + i, v)
                if vehicle.return_to_depot
                else 0
            )
            model.add(arrival[i] >= vehicle.departure_time + from_home).only_enforce_if(
                first
            )
            model.add(
                route_end
                == service_start[i] + data.visits[i].service_duration + to_home
            ).only_enforce_if(last)
            latency = model.new_int_var(0, horizon, f"vehicle_{v}_visit_{i}_latency")
            model.add(latency == arrival[i] - vehicle.departure_time).only_enforce_if(
                assigned[v, i]
            )
            model.add(latency == 0).only_enforce_if(assigned[v, i].Not())
            latency_terms.append(latency)
            route_travel_terms.extend([from_home * first, to_home * last])
        for i in range(visit_count):
            for j in range(visit_count):
                if i == j:
                    continue
                arc = model.new_bool_var(f"vehicle_{v}_arc_{i}_{j}")
                arcs.append((i + 1, j + 1, arc))
                drive = plan.driving_time(vehicle_count + i, vehicle_count + j)
                model.add(
                    arrival[j]
                    >= service_start[i] + data.visits[i].service_duration + drive
                ).only_enforce_if(arc)
                route_travel_terms.append(drive * arc)
        model.add_circuit(arcs)
        route_arcs[v] = arcs
        travel_terms.extend(route_travel_terms)
        vehicle_travel[v] = sum(route_travel_terms)
        vehicle_end[v] = route_end
        vehicle_latency[v] = sum(latency_terms)
        vehicle_demand[v] = sum(
            data.visits[i].demand * assigned[v, i] for i in range(visit_count)
        )
        radius_values = [
            _haversine_meters(vehicle.depot, visit.location) for visit in data.visits
        ]
        radii = [radius_values[i] * assigned[v, i] for i in range(visit_count)]
        furthest = model.new_int_var(
            0, max(radius_values, default=0), f"vehicle_{v}_route_radius"
        )
        model.add_max_equality(furthest, [0, *radii])
        vehicle_radius[v] = furthest

    if _fixed_routes is not None:
        _fix_routes(
            model,
            route_arcs,
            [vehicle.id for vehicle in data.vehicles],
            [visit.id for visit in data.visits],
            _fixed_routes,
        )

    REGISTRY.build(
        RoutingContext(
            model,
            data,
            plan,
            horizon,
            assigned,
            unassigned,
            arrival,
            travel_terms,
            vehicle_used,
            vehicle_travel,
            vehicle_end,
            vehicle_latency,
            vehicle_stop_count,
            vehicle_demand,
            vehicle_radius,
        ),
        objective,
    )
    objective.minimize()

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = request.timelimit
    solver.parameters.random_seed = request.random_seed
    solver.parameters.num_search_workers = request.num_workers
    status = solver.solve(model)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(
            f"CP-SAT did not find a solution (status={solver.status_name(status)})"
        )

    routes: dict[str, list[VisitStop]] = {}
    for v, vehicle in enumerate(data.vehicles):
        selected = {i for i in range(visit_count) if solver.value(assigned[v, i])}
        successors = {
            tail - 1: head - 1
            for tail, head, literal in route_arcs[v]
            if tail != head and solver.value(literal)
        }
        order = []
        current = successors.get(-1, -1)
        while current >= 0 and current in selected:
            order.append(current)
            current = successors.get(current, -1)
        routes[vehicle.id] = [
            VisitStop(
                visit_id=data.visits[i].id,
                arrival_time=solver.value(arrival[i]),
                demand=data.visits[i].demand,
            )
            for i in order
        ]
    unassigned_ids = [
        data.visits[i].id for i in range(visit_count) if solver.value(unassigned[i])
    ]
    score = objective.score(solver)
    hard = score["hard"]
    medium = score["medium"]
    # Per-vehicle figures are post-processed from the routes exactly like the
    # C++ backend does, so both report the same numbers for the same routes.
    metrics = vehicle_metrics(plan, routes)
    return RouteSolution(
        routes=routes,
        unassigned=unassigned_ids,
        score=f"{hard}hard/{medium}medium/{score['soft']}soft",
        feasible=hard == 0,
        total_driving_seconds=sum(m.driving_seconds for m in metrics.values()),
        vehicle_metrics=metrics,
    )
