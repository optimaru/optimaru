"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`Solver(cfg).solve(plan)`) enables the seven core PDPTW constraints;
the per-vehicle route-policy constraints cost 0 while their parameter is 0. Duration and
latency are opt-in objectives with default weight 0.
This module is the alternative path next to it: pick constraints, change weights, flip
hard/soft, and pass the list to `solve_with_constraints`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import pdp_routing as pdp
    from pdp_routing.constraints import (
        MinimizeTravelTime, get_constraints, vehicle_capacity,
    )

    solver = pdp.Solver(cfg)
    solver.solve_with_constraints(plan, get_constraints())      # defaults as-is
    solver.solve_with_constraints(plan, get_constraints([       # picked
        vehicle_capacity, MinimizeTravelTime(weight=10),
    ]))

Each constraint has a default instance at module level, so you can also pass a
list of just a few. Only listed constraints are scored — leave one out and its
term disappears from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = component default.
- ``hard``     ``True``/``False`` flips the default level; ``None`` keeps it.
               **Medium constraints in this domain cannot be expressed with
               this knob** — ``hard=False`` folds them into soft and the medium
               meaning is lost. Leave medium terms at ``None``.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. 2.0 = squared, so concentrated violations grow much costlier.

The three coupling constraints (`paired_request`, `same_vehicle`, `pickup_before_dropoff`)
are cut so they never overlap — each situation is valued by exactly one term.

Names (`name`) are the C++ component names in snake_case. The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`pdp.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.

`parallel_solver_count` does not apply on the plugin path (a caller-owned
calculator cannot be cloned into workers). The pair-aware move/ruin operators read
the plan, not the calculator, so they still run.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "VehicleCapacity",
    "ServiceFinishedAfterMaxEndTime",
    "MinimizeTravelTime",
    "PairedRequest",
    "SameVehicle",
    "PickupBeforeDropoff",
    "PinnedStops",
    "RequiredVehicle",
    "UnassignedRequests",
    "MaxTravelTime",
    "MaxRouteDuration",
    "VehicleEndTime",
    "MaxStops",
    "MaxRouteRadius",
    "MinimizeEnclosingCircleRadius",
    "MinCapacityUtilization",
    "MinimizeRouteDuration",
    "MinimizeLatency",
    "MinimizeLateness",
    "VehicleFixedCost",
    "vehicle_capacity",
    "service_finished_after_max_end_time",
    "minimize_travel_time",
    "paired_request",
    "same_vehicle",
    "pickup_before_dropoff",
    "pinned_stops",
    "required_vehicle",
    "unassigned_requests",
    "max_travel_time",
    "max_route_duration",
    "vehicle_end_time",
    "max_stops",
    "max_route_radius",
    "minimize_enclosing_circle_radius",
    "min_capacity_utilization",
    "minimize_route_duration",
    "minimize_latency",
    "minimize_lateness",
    "vehicle_fixed_cost",
    "get_constraints",
]


@dataclass(frozen=True)
class Constraint:
    """Spec selecting one constraint: weight / level / shaping."""

    weight: float | None = None  # None = component default weight
    hard: bool | None = None  # None = component default level
    exponent: float = 1.0
    name: ClassVar[str] = ""
    label_en: ClassVar[str] = ""
    description_en: ClassVar[str] = ""

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} must set a registry `name`")


class VehicleCapacity(Constraint):
    """Load accumulated along the route above vehicle capacity. Default: hard, weight 1.

    Goods board at the pickup and leave at the matching dropoff, so what must fit
    is the **peak** in between, not the route total. The excess is charged at
    every node where it persists — exactly like a full recompute."""

    name = "vehicle_capacity"
    label_en = "Vehicle Capacity"
    description_en = "Penalizes the running load (up at pickups, down at dropoffs) above the vehicle capacity. Default: **hard**, weight 1."


class ServiceFinishedAfterMaxEndTime(Constraint):
    """Minutes (rounded up) by which node service ends after its time window. Default: hard, weight 1."""

    name = "service_finished_after_max_end_time"
    label_en = "Service Finished After Max End Time"
    description_en = "Penalizes rounded-up minutes by which a stop finishes after its time window. Default: **hard**, weight 1. It uses **arrival time + service duration**."


class MinimizeTravelTime(Constraint):
    """Round-trip driving seconds per vehicle (depot -> nodes -> depot). Default: soft, weight 1.

    An idle vehicle drives nothing, so it costs nothing."""

    name = "minimize_travel_time"
    label_en = "Minimize Travel Time"
    description_en = "Minimizes each vehicle's driving time from its home through its stops; the leg back home counts only when the vehicle's `return_to_depot` is on. Default: **soft**, weight 1."


class PairedRequest(Constraint):
    """Requests with only one node on a route. Default: hard, weight 1000.

    Half a delivery is worse than none — goods get loaded and never dropped off,
    or dropped off from nowhere."""

    name = "paired_request"
    label_en = "Paired Request"
    description_en = "Penalizes requests with only one of pickup and dropoff routed. Default: **hard**, weight 1000."


class SameVehicle(Constraint):
    """Requests whose pickup and dropoff ride on **different** vehicles. Default: hard, weight 1000.

    Nobody hands goods over midway, so both nodes must travel together.
    Silent when only one or neither is assigned — `paired_request` and
    `unassigned_requests` value those."""

    name = "same_vehicle"
    label_en = "Same Vehicle"
    description_en = "Penalizes requests whose pickup and dropoff ride on different vehicles. Default: **hard**, weight 1000."


class PickupBeforeDropoff(Constraint):
    """Requests whose dropoff precedes the pickup on the same route. Default: hard, weight 1000.

    Only looks at both nodes on the same vehicle — split cases are valued by
    `same_vehicle`."""

    name = "pickup_before_dropoff"
    label_en = "Pickup Before Dropoff"
    description_en = "Penalizes requests whose dropoff precedes its pickup in the same route. Default: **hard**, weight 1000."


class UnassignedRequests(Constraint):
    """Service minutes of requests with **neither** node assigned. Default: medium, weight 10^9.

    Dropping a long job hurts more than dropping a short one. Half-assigned
    requests are not counted here — `paired_request` values them at the hard level.
    The medium level is folded into the engine soft level as `medium * kMediumUnit`."""

    name = "unassigned_requests"
    label_en = "Unassigned Requests"
    description_en = "Penalizes the service minutes of requests left entirely unrouted. Default: **medium**, weight `10^9`."


class MaxTravelTime(Constraint):
    """Seconds of round-trip travel above the vehicle limit."""

    name = "max_travel_time"
    label_en = "Maximum Travel Time"
    description_en = "Penalizes driving time above `Vehicle.max_travel_time` (the leg home counts only with `return_to_depot`)."


class MaxRouteDuration(Constraint):
    """Seconds of vehicle working time, waits and service included, above the limit."""

    name = "max_route_duration"
    label_en = "Maximum Route Duration"
    description_en = "Penalizes route duration above `Vehicle.max_route_duration`, including waiting and service (and the return leg only with `return_to_depot`)."


class VehicleEndTime(Constraint):
    """Seconds by which the depot return passes the vehicle end time."""

    name = "vehicle_end_time"
    label_en = "Vehicle End Time"
    description_en = "Penalizes the route end time (last-stop departure, or depot return with `return_to_depot`) after `Vehicle.max_end_time`."


class MaxStops(Constraint):
    """Pickup/dropoff stops per vehicle above the limit."""

    name = "max_stops"
    label_en = "Maximum Stops"
    description_en = "Penalizes pickup and dropoff stops above `Vehicle.max_stops` (a request counts as two). Default 20; 0 lifts the cap."


class MaxRouteRadius(Constraint):
    """Meters of route radius from the depot above the limit."""

    name = "max_route_radius"
    label_en = "Maximum Route Radius"
    description_en = "Penalizes the farthest stop's straight-line distance beyond `Vehicle.max_route_radius_meters`."


class MinimizeEnclosingCircleRadius(Constraint):
    """Radius (meters) of the minimum enclosing circle of the pickup/dropoff points.

    The depot is excluded; an opt-in objective with default weight=0.
    """

    name = "minimize_enclosing_circle_radius"
    label_en = "Minimize Enclosing Circle Radius"
    description_en = "Minimizes the smallest enclosing-circle radius of each route's stops. The default weight is 0, and the home is not included in the circle."


class MinCapacityUtilization(Constraint):
    """Shortfall of used vehicles below the target minimum throughput. Default soft."""

    name = "min_capacity_utilization"
    label_en = "Minimum Capacity Utilization"
    description_en = "Penalizes used vehicles whose delivered demand is below the `Vehicle.min_load_ratio` target."


class MinimizeRouteDuration(Constraint):
    """Total route time including waits and service. Default weight=0, so opt-in."""

    name = "minimize_route_duration"
    label_en = "Minimize Route Duration"
    description_en = "Minimizes total route duration including waiting and service (and the return leg only with `return_to_depot`). The default weight is 0."


class MinimizeLatency(Constraint):
    """Sum of the times taken to reach each stop. Default weight=0, so opt-in."""

    name = "minimize_latency"
    label_en = "Minimize Latency"
    description_en = "Minimizes total time from each vehicle's departure to every stop arrival. The default weight is 0."


class PinnedStops(Constraint):
    """Routes that do not start with their vehicle's pinned stops. Default: hard, weight 1e12.

    Each misplaced stop counts once. Silent for a vehicle that pins nothing. The weight is far above
    what the other hard rules total on real input, so no trade between them moves a pinned stop."""

    name = "pinned_stops"
    label_en = "Pinned Stops"
    description_en = "Penalizes routes that do not start with the vehicle's pinned stops, in order. Default: **hard**, weight 1e12."


class RequiredVehicle(Constraint):
    """Requests off the vehicle they must ride on. Default: hard, weight 1000.

    Unassigned counts once and another vehicle twice, so a request with no place on its vehicle waits.
    Where on the route the request goes stays free."""

    name = "required_vehicle"
    label_en = "Required Vehicle"
    description_en = "Penalizes requests that are not on their `required_vehicle`: once when unassigned, twice on another vehicle. Default: **hard**, weight 1000."


class MinimizeLateness(Constraint):
    """Squared seconds from each order to its dropoff finishing. Default weight=0, so opt-in.

    Only dropoffs count, and each request counts on its own, so one very late
    order costs more than several slightly late ones. An unassigned request is
    charged as if finished an hour into the plan: otherwise leaving out the
    oldest orders would be the cheapest thing to do."""

    name = "minimize_lateness"
    label_en = "Minimize Lateness"
    description_en = (
        "Minimizes the sum, over requests, of the squared seconds from `order_time` to "
        "the end of the dropoff. The default weight is 0."
    )


class VehicleFixedCost(Constraint):
    """Fixed cost of every vehicle used at least once."""

    name = "vehicle_fixed_cost"
    label_en = "Vehicle Fixed Cost"
    description_en = (
        "Adds `Vehicle.fixed_cost` for every vehicle that is used at least once."
    )


#: Instances at the default weights. Reference them directly when picking.
vehicle_capacity = VehicleCapacity()
service_finished_after_max_end_time = ServiceFinishedAfterMaxEndTime()
minimize_travel_time = MinimizeTravelTime()
paired_request = PairedRequest()
same_vehicle = SameVehicle()
pickup_before_dropoff = PickupBeforeDropoff()
unassigned_requests = UnassignedRequests()
max_travel_time = MaxTravelTime()
max_route_duration = MaxRouteDuration()
vehicle_end_time = VehicleEndTime()
max_stops = MaxStops()
max_route_radius = MaxRouteRadius()
minimize_enclosing_circle_radius = MinimizeEnclosingCircleRadius(weight=0)
min_capacity_utilization = MinCapacityUtilization()
minimize_route_duration = MinimizeRouteDuration()
minimize_latency = MinimizeLatency()
minimize_lateness = MinimizeLateness()
pinned_stops = PinnedStops()
required_vehicle = RequiredVehicle()
vehicle_fixed_cost = VehicleFixedCost()

#: Available set — same order as `pdp.list_constraints()`.
_DEFAULTS: list[Constraint] = [
    vehicle_capacity,
    service_finished_after_max_end_time,
    minimize_travel_time,
    max_travel_time,
    max_route_duration,
    vehicle_end_time,
    max_stops,
    max_route_radius,
    minimize_enclosing_circle_radius,
    min_capacity_utilization,
    minimize_route_duration,
    minimize_latency,
    minimize_lateness,
    vehicle_fixed_cost,
    pinned_stops,
    required_vehicle,
    paired_request,
    same_vehicle,
    pickup_before_dropoff,
    unassigned_requests,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, every registered constraint.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([vehicle_capacity, MinimizeTravelTime(weight=10)])
    """
    if constraints is None:
        return list(_DEFAULTS)
    if not constraints:
        # Same contract as C++ applyConstraints: an empty list means "score
        # nothing", which is most likely a mistake. A filter that happens to come
        # out empty must not silently flip to "score everything".
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return list(constraints)


class Score(NamedTuple):
    """(hard, soft) returned by `score()`. Both are penalties, so <= 0."""

    hard: int
    soft: float


def _shape(cost: float, exponent: float) -> float:
    """Sign-preserving power. Rewards (negative costs) never flip into penalties."""
    if exponent == 1.0 or cost == 0.0:
        return cost
    return -((-cost) ** exponent) if cost < 0.0 else cost**exponent


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `pdp.constraint_costs(plan)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = pdp.constraint_costs(plan)
        score(raw)                     # default set
        score(raw, picked)             # picked list

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft). This domain folds
    medium into engine soft as multiples of kMediumUnit (10^9), so to compare
    with the split levels of `solution.score`, fold the same way first.
    """
    hard = 0
    soft = 0.0
    for spec in get_constraints(constraints):
        entry = costs.get(spec.name)
        if entry is None:
            raise ValueError(
                f"unknown constraint {spec.name!r}; available: "
                + ", ".join(repr(name) for name in sorted(costs))
            )
        weight = entry["weight"] if spec.weight is None else spec.weight
        is_hard = entry["hard"] if spec.hard is None else spec.hard
        exponent = spec.exponent
        for cost in entry["costs"]:
            value = weight * _shape(cost, exponent)
            if is_hard:
                # Round per unit contribution like the engine (half-away-from-zero
                # like llround; Python's round is banker's rounding).
                hard += int(
                    math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5)
                )
            else:
                soft += value
    return Score(-hard, -soft if soft != 0.0 else 0.0)
