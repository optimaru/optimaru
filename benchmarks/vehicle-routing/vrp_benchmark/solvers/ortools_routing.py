"""Google OR-Tools Routing solver (`pywrapcp`), the library's recommended VRP
setup: PATH_CHEAPEST_ARC first solution, then guided local search."""

from __future__ import annotations

from ortools.constraint_solver import pywrapcp, routing_enums_pb2

from ..instance import CvrpInstance
from . import SolverOutcome


def solve(instance: CvrpInstance, seconds: float, seed: int) -> SolverOutcome:
    # The Routing solver exposes no random seed; its search is deterministic
    # up to the wall-clock cut-off.
    del seed
    node_count = len(instance.demands)
    manager = pywrapcp.RoutingIndexManager(node_count, instance.vehicle_count, 0)
    routing = pywrapcp.RoutingModel(manager)
    distances = instance.distances
    demands = instance.demands

    def distance(from_index: int, to_index: int) -> int:
        return distances[manager.IndexToNode(from_index)][manager.IndexToNode(to_index)]

    def demand(from_index: int) -> int:
        return demands[manager.IndexToNode(from_index)]

    transit = routing.RegisterTransitCallback(distance)
    routing.SetArcCostEvaluatorOfAllVehicles(transit)
    routing.AddDimensionWithVehicleCapacity(
        routing.RegisterUnaryTransitCallback(demand),
        0,
        [instance.capacity] * instance.vehicle_count,
        True,
        "Capacity",
    )
    # On tightly loaded instances the first-solution heuristic may not find a
    # complete plan; a dropped customer costs more than any route can save,
    # so it is a last resort, and the harness reports it as unassigned.
    drop_penalty = max(map(max, distances)) * node_count * 2
    for node in range(1, node_count):
        routing.AddDisjunction([manager.NodeToIndex(node)], drop_penalty)

    parameters = pywrapcp.DefaultRoutingSearchParameters()
    parameters.first_solution_strategy = (
        routing_enums_pb2.FirstSolutionStrategy.PATH_CHEAPEST_ARC
    )
    parameters.local_search_metaheuristic = (
        routing_enums_pb2.LocalSearchMetaheuristic.GUIDED_LOCAL_SEARCH
    )
    parameters.time_limit.FromMilliseconds(int(seconds * 1000))

    assignment = routing.SolveWithParameters(parameters)
    if assignment is None:
        return SolverOutcome(routes=[], reported_distance=None, summary="no solution")
    routes: list[list[int]] = []
    for vehicle in range(instance.vehicle_count):
        index = routing.Start(vehicle)
        nodes = []
        while not routing.IsEnd(index):
            node = manager.IndexToNode(index)
            if node != 0:
                nodes.append(node)
            index = assignment.Value(routing.NextVar(index))
        routes.append(nodes)
    # ObjectiveValue includes drop penalties, so it equals the distance only
    # when every customer is routed.
    dropped = node_count - 1 - sum(map(len, routes))
    objective = assignment.ObjectiveValue()
    return SolverOutcome(
        routes=routes,
        reported_distance=objective if dropped == 0 else None,
        summary=f"objective={objective} dropped={dropped}",
    )
