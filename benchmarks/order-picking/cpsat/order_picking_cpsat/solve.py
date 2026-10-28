"""Multiple-circuit CP-SAT formulation for warehouse order picking."""

from __future__ import annotations

from ortools.sat.python import cp_model

from cpsat_plugins import ConstraintConfig
from order_picking import _core, constraint_costs
from order_picking.constraints import Constraint
from order_picking.models import (
    OrderPickingRequest,
    OrderPickingResponse,
    PickStop,
    to_solution,
)

from .constraints import OrderPickingContext, REGISTRY

__all__ = ["catalog", "solve", "solve_request"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def solve_request(
    request: OrderPickingRequest,
    *,
    constraints: list[ConstraintConfig | Constraint | str | dict] | None = None,
    _fixed_routes: list[list[int]] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> OrderPickingResponse:
    model = cp_model.CpModel()
    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)
    trolleys = request.trolleys
    items = [
        (order_index, item_index, order, item)
        for order_index, order in enumerate(request.orders)
        for item_index, item in enumerate(order.items)
    ]
    trolley_count, task_count = len(trolleys), len(items)
    assigned = {
        (t, i): model.new_bool_var(f"trolley_{t}_task_{i}")
        for t in range(trolley_count)
        for i in range(task_count)
    }
    for i in range(task_count):
        model.add_exactly_one(assigned[t, i] for t in range(trolley_count))

    task_locations = [item.location.to_core() for _o, _i, _order, item in items]
    trolley_locations = [trolley.location.to_core() for trolley in trolleys]
    between = {
        (i, j): _core.calculate_distance(task_locations[i], task_locations[j])
        for i in range(task_count)
        for j in range(task_count)
        if i != j
    }
    route_arcs: dict[int, list[tuple[int, int, object]]] = {}
    first_arcs = {}
    last_arcs = {}
    between_arcs = {}
    order_used = {}
    overflows = []
    for t, trolley in enumerate(trolleys):
        empty = model.new_bool_var(f"trolley_{t}_empty")
        selected_count = sum(assigned[t, i] for i in range(task_count))
        model.add(selected_count == 0).only_enforce_if(empty)
        model.add(selected_count >= 1).only_enforce_if(empty.Not())
        arcs: list[tuple[int, int, object]] = [(0, 0, empty)]
        for i in range(task_count):
            node = i + 1
            arcs.append((node, node, assigned[t, i].Not()))
            first = model.new_bool_var(f"trolley_{t}_first_{i}")
            last = model.new_bool_var(f"trolley_{t}_last_{i}")
            arcs.extend(((0, node, first), (node, 0, last)))
            first_arcs[t, i] = first
            last_arcs[t, i] = last
        for i in range(task_count):
            for j in range(task_count):
                if i == j:
                    continue
                arc = model.new_bool_var(f"trolley_{t}_arc_{i}_{j}")
                arcs.append((i + 1, j + 1, arc))
                between_arcs[t, i, j] = arc
        model.add_circuit(arcs)
        route_arcs[t] = arcs

        required_buckets = []
        for o, order in enumerate(request.orders):
            indices = [
                i for i, (order_index, *_rest) in enumerate(items) if order_index == o
            ]
            total_volume = sum(items[i][3].volume * assigned[t, i] for i in indices)
            max_volume = sum(items[i][3].volume for i in indices)
            numerator = model.new_int_var(
                0,
                max_volume + trolley.bucket_capacity - 1,
                f"trolley_{t}_order_{o}_numerator",
            )
            buckets = model.new_int_var(
                0,
                (max_volume + trolley.bucket_capacity - 1) // trolley.bucket_capacity,
                f"trolley_{t}_order_{o}_buckets",
            )
            model.add(numerator == total_volume + trolley.bucket_capacity - 1)
            model.add_division_equality(buckets, numerator, trolley.bucket_capacity)
            used = model.new_bool_var(f"trolley_{t}_order_{o}_used")
            model.add_max_equality(used, [assigned[t, i] for i in indices])
            # ceil(0/capacity) must be zero; the shifted division is one otherwise.
            model.add(buckets == 0).only_enforce_if(used.Not())
            required_buckets.append(buckets)
            order_used[t, o] = used
        overflow = model.new_int_var(
            0,
            sum(len(order.items) for order in request.orders),
            f"trolley_{t}_bucket_overflow",
        )
        model.add_max_equality(
            overflow, [0, sum(required_buckets) - trolley.bucket_count]
        )
        overflows.append(overflow)

    if _fixed_routes is not None:
        if len(_fixed_routes) != trolley_count:
            raise ValueError("_fixed_routes must contain one route per trolley")
        flattened = [task for route in _fixed_routes for task in route]
        if sorted(flattened) != list(range(task_count)):
            raise ValueError("_fixed_routes must contain every task exactly once")
        for t, route in enumerate(_fixed_routes):
            selected = set(route)
            for i in range(task_count):
                model.add(assigned[t, i] == (i in selected))
            expected_arcs = {
                (i + 1, i + 1) for i in range(task_count) if i not in selected
            }
            expected_arcs.update(
                {(0, 0)}
                if not route
                else {
                    (0, route[0] + 1),
                    *((left + 1, right + 1) for left, right in zip(route, route[1:])),
                    (route[-1] + 1, 0),
                }
            )
            for tail, head, literal in route_arcs[t]:
                model.add(literal == ((tail, head) in expected_arcs))

    REGISTRY.build(
        OrderPickingContext(
            model,
            request,
            task_locations,
            trolley_locations,
            first_arcs,
            last_arcs,
            between_arcs,
            between,
            order_used,
            overflows,
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
    if _proxy_score is not None:
        _proxy_score.update(objective.score(solver))

    routes: dict[str, list[PickStop]] = {}
    distance_by_trolley: dict[str, int] = {}
    total_distance = 0
    for t, trolley in enumerate(trolleys):
        successors = {
            tail - 1: head - 1
            for tail, head, literal in route_arcs[t]
            if tail != head and solver.value(literal)
        }
        order_indices = []
        current = successors.get(-1, -1)
        while current >= 0:
            order_indices.append(current)
            current = successors.get(current, -1)
        stops = []
        for i in order_indices:
            _order_index, item_index, order, item = items[i]
            stops.append(
                PickStop(
                    pick_task_id=f"{order.id}-{item_index}",
                    order_id=order.id,
                    product_name=item.name or item.id,
                    volume=item.volume,
                    location=item.location,
                )
            )
        routes[trolley.id] = stops
        if order_indices:
            distance = _core.calculate_distance(
                trolley_locations[t], task_locations[order_indices[0]]
            )
            distance += sum(
                between[left, right]
                for left, right in zip(order_indices, order_indices[1:])
            )
            distance += _core.calculate_distance(
                task_locations[order_indices[-1]], trolley_locations[t]
            )
        else:
            distance = 0
        distance_by_trolley[trolley.id] = distance
        total_distance += distance
    canonical = to_solution(request)
    task_by_id = {task.id: i for i, task in enumerate(canonical.pick_tasks)}
    for trolley in canonical.trolleys:
        trolley.pick_task_indices = [
            task_by_id[stop.pick_task_id] for stop in routes[trolley.id]
        ]
    score = objective.score_costs(constraint_costs(canonical))
    split_count = sum(
        len({stop.order_id for stop in stops}) for stops in routes.values()
    )
    return OrderPickingResponse(
        routes=routes,
        distance_by_trolley=distance_by_trolley,
        score=f"{score['hard']}hard/{score['soft']}soft",
        feasible=score["hard"] == 0,
        total_distance=total_distance,
        order_split_count=split_count,
    )


solve = solve_request
