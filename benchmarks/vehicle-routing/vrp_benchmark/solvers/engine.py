"""optimaru's vehicle-routing use case (`optimaru.vehicle_routing`), default configuration."""

from __future__ import annotations

from optimaru.vehicle_routing.models import (
    RoutePlanRequest,
    RoutingInput,
    VehicleModel,
    VisitModel,
    solve_request,
)

from ..instance import CvrpInstance
from . import SolverOutcome

# CVRP: a nonzero service time per visit; it never binds (`_cvrp_windows`).
SERVICE_SECONDS = 60


def _expanded_matrix(instance: CvrpInstance) -> list[list[int]]:
    # The request orders one depot location per vehicle, then all customers.
    original = [0] * instance.vehicle_count + list(range(1, len(instance.demands)))
    return [[instance.distances[i][j] for j in original] for i in original]


def make_request(instance: CvrpInstance, *, seconds: float, seed: int) -> RoutePlanRequest:
    if instance.time_windows:
        windows = _vrptw_windows(instance)
        departure, closing = instance.ready_times[0], instance.due_times[0]
    else:
        windows = _cvrp_windows(instance)
        departure, closing = 0, 0
    depot = instance.coordinates[0]
    return RoutePlanRequest(
        name=instance.name,
        time_limit_seconds=seconds,
        random_seed=seed,
        parallel_solver_count=1,
        model_input=RoutingInput(
            vehicles=[
                VehicleModel(
                    id=f"V{i + 1}",
                    capacity=instance.capacity,
                    depot=(float(depot[1]), float(depot[0])),
                    # Both sets score the round trip, so close every route.
                    return_to_depot=True,
                    departure_time=departure,
                    # 0 is no limit (CVRP); on VRPTW the depot's closing time.
                    max_end_time=closing,
                )
                for i in range(instance.vehicle_count)
            ],
            visits=[
                VisitModel(
                    id=str(i),
                    location=(float(y), float(x)),
                    demand=instance.demands[i],
                    min_start_time=windows[i][0],
                    max_end_time=windows[i][1],
                    service_duration=windows[i][2],
                    unassigned_penalty_minutes=1,
                )
                for i, (x, y) in enumerate(instance.coordinates[1:], start=1)
            ],
            driving_seconds=_expanded_matrix(instance),
        ),
    )


def _cvrp_windows(instance: CvrpInstance) -> dict[int, tuple[int, int, int]]:
    """(earliest start, latest service end, service) per customer, none of which can bind.

    Every deadline lies past the longest possible route (every leg at the
    longest distance, every service included), so the engine minimises exactly
    the CVRPLIB distance.
    """
    customer_count = instance.customer_count
    deadline = max(map(max, instance.distances)) * (customer_count + 1) + SERVICE_SECONDS * customer_count + 1
    return {i: (0, deadline, SERVICE_SECONDS) for i in range(1, customer_count + 1)}


def _vrptw_windows(instance: CvrpInstance) -> dict[int, tuple[int, int, int]]:
    """The instance's windows, in the engine's terms.

    The benchmark's window bounds the start of service; the engine's
    `max_end_time` bounds arrival + service duration. With the window's end
    moved by the service time the two agree: a vehicle that arrives before
    the window waits and starts on time, so a service starts late exactly
    when the vehicle arrives after the window's end.
    """
    assert instance.ready_times and instance.due_times and instance.service_times
    return {
        i: (instance.ready_times[i], instance.due_times[i] + instance.service_times[i], instance.service_times[i])
        for i in range(1, instance.customer_count + 1)
    }


def solve(instance: CvrpInstance, seconds: float, seed: int) -> SolverOutcome:
    # Monolithic fast path: the default constraints are exactly the CVRP and
    # VRPTW objective here (travel time is the distance), and monolith-vs-plugin
    # score parity is a standing vrp_plugin_test gate.
    solution = solve_request(make_request(instance, seconds=seconds, seed=seed))
    return SolverOutcome(
        routes=[[int(stop.visit_id) for stop in stops] for stops in solution.routes.values()],
        reported_distance=solution.total_driving_seconds,
        summary=str(solution.score),
    )
