"""optimaru's vehicle-routing use case (`vrp_routing`), default configuration."""

from __future__ import annotations

from vrp_routing.models import (
    RoutePlanRequest,
    RoutingInput,
    VehicleModel,
    VisitModel,
    solve_request,
)

from ..instance import CvrpInstance
from . import SolverOutcome

# A nonzero service time per visit; it never binds (see the deadline below).
SERVICE_SECONDS = 60


def _expanded_matrix(instance: CvrpInstance) -> list[list[int]]:
    # The request orders one depot location per vehicle, then all customers.
    original = [0] * instance.vehicle_count + list(range(1, len(instance.demands)))
    return [[instance.distances[i][j] for j in original] for i in original]


def make_request(instance: CvrpInstance, *, seconds: float, seed: int) -> RoutePlanRequest:
    customer_count = instance.customer_count
    # No time window can bind: every deadline lies past the longest possible
    # route (every leg at the longest distance, every service included), so
    # the engine minimises exactly the CVRPLIB distance.
    deadline = (
        max(map(max, instance.distances)) * (customer_count + 1)
        + SERVICE_SECONDS * customer_count
        + 1
    )
    depot = instance.coordinates[0]
    return RoutePlanRequest(
        name=instance.name,
        timelimit=seconds,
        random_seed=seed,
        num_workers=1,
        model_input=RoutingInput(
            vehicles=[
                VehicleModel(
                    id=f"V{i + 1}",
                    capacity=instance.capacity,
                    depot=(float(depot[1]), float(depot[0])),
                    # CVRPLIB scores the round trip, so close every route.
                    return_to_depot=True,
                )
                for i in range(instance.vehicle_count)
            ],
            visits=[
                VisitModel(
                    id=str(i),
                    location=(float(y), float(x)),
                    demand=instance.demands[i],
                    max_end_time=deadline,
                    service_duration=SERVICE_SECONDS,
                    unassigned_penalty_minutes=1,
                )
                for i, (x, y) in enumerate(instance.coordinates[1:], start=1)
            ],
            driving_seconds=_expanded_matrix(instance),
        ),
    )


def solve(instance: CvrpInstance, seconds: float, seed: int) -> SolverOutcome:
    # Monolithic fast path: the default constraints are exactly the CVRP
    # objective here, and monolith-vs-plugin score parity is a standing
    # vrp_plugin_test gate.
    solution = solve_request(make_request(instance, seconds=seconds, seed=seed))
    return SolverOutcome(
        routes=[[int(stop.visit_id) for stop in stops] for stops in solution.routes.values()],
        reported_distance=solution.total_driving_seconds,
        summary=str(solution.score),
    )
