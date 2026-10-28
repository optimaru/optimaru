"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`Solver(cfg).solve(plan)`) enables the four quickstart constraints;
the per-vehicle route-policy constraints cost 0 while their parameter is 0. Duration and
latency are opt-in objectives with default weight 0.
This module is the alternative path next to it: pick constraints, change weights, flip
hard/soft, and pass the list to `solve_with_constraints`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import vrp_routing as vrp
    from vrp_routing.constraints import (
        MinimizeTravelTime, get_constraints, vehicle_capacity,
    )

    solver = vrp.Solver(cfg)
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

Names (`name`) are the C++ component names in snake_case. The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`vrp.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.

Two knobs do not apply on the plugin path: `parallel_solver_count` (a caller-owned
calculator cannot be cloned into workers) and the driving-time-guided selection.
Details are in the comments of `cpp/include/vrp/solver.hpp`.
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
    "MaximizeVisitsAssigned",
    "MaxTravelTime",
    "MaxRouteDuration",
    "VehicleEndTime",
    "MaxStops",
    "MaxRouteRadius",
    "MinimizeEnclosingCircleRadius",
    "MinCapacityUtilization",
    "MinimizeRouteDuration",
    "MinimizeLatency",
    "VehicleFixedCost",
    "vehicle_capacity",
    "service_finished_after_max_end_time",
    "minimize_travel_time",
    "maximize_visits_assigned",
    "max_travel_time",
    "max_route_duration",
    "vehicle_end_time",
    "max_stops",
    "max_route_radius",
    "minimize_enclosing_circle_radius",
    "min_capacity_utilization",
    "minimize_route_duration",
    "minimize_latency",
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
    """Demand loaded beyond what the vehicle can carry. Default: hard, weight 1."""

    name = "vehicle_capacity"
    label_en = "Vehicle Capacity"
    description_en = (
        "Penalizes vehicle load above its capacity. Default: **hard**, weight 1."
    )


class ServiceFinishedAfterMaxEndTime(Constraint):
    """Minutes (rounded up) by which visit service ends after its time window. Default: hard, weight 1.

    Measured from **arrival**, not service start — exactly as the Java quickstart."""

    name = "service_finished_after_max_end_time"
    label_en = "Service Finished After Max End Time"
    description_en = (
        "Penalizes rounded-up minutes by which a visit finishes after its time window. "
        "Default: **hard**, weight 1. It uses **arrival time + service duration**, not service start."
    )


class MinimizeTravelTime(Constraint):
    """Round-trip driving time per vehicle (depot -> visits -> depot). Default: soft, weight 1.

    An idle vehicle drives nothing, so it costs nothing."""

    name = "minimize_travel_time"
    label_en = "Minimize Travel Time"
    description_en = "Minimizes each vehicle's driving time from its depot through its visits; the leg back counts only when the vehicle's `return_to_depot` is on. Default: **soft**, weight 1."


class MaximizeVisitsAssigned(Constraint):
    """Penalty of visits left off every route. Default: medium, weight 10^9.

    The per-visit penalty defaults to the service time in minutes
    (`Visit.unassigned_penalty`). Unassigned visits belong to no unit, so this is
    a global constraint. The medium level is folded into the engine soft level as
    `medium * kMediumUnit`, so the default weight carries that multiple."""

    name = "maximize_visits_assigned"
    label_en = "Maximize Visits Assigned"
    description_en = (
        "Penalizes each unassigned visit by `Visit.unassigned_penalty`. "
        "Default: **medium**, weight `10^9`."
    )


class MaxTravelTime(Constraint):
    """Round-trip driving seconds of a vehicle above ``Vehicle.max_travel_time``."""

    name = "max_travel_time"
    label_en = "Maximum Travel Time"
    description_en = "Penalizes driving time above `Vehicle.max_travel_time` (the leg home counts only with `return_to_depot`)."


class MaxRouteDuration(Constraint):
    """Working time, waits, service and return included, above the limit."""

    name = "max_route_duration"
    label_en = "Maximum Route Duration"
    description_en = "Penalizes route duration above the vehicle limit, including waiting and service (and the return leg only with `return_to_depot`)."


class VehicleEndTime(Constraint):
    """Seconds by which the depot return passes ``Vehicle.max_end_time``."""

    name = "vehicle_end_time"
    label_en = "Vehicle End Time"
    description_en = "Penalizes the route end time (last-visit departure, or depot return with `return_to_depot`) after `Vehicle.max_end_time`."


class MaxStops(Constraint):
    """Visits per vehicle above the maximum."""

    name = "max_stops"
    label_en = "Maximum Stops"
    description_en = "Penalizes the number of stops above `Vehicle.max_stops`."


class MaxRouteRadius(Constraint):
    """Meters by which the farthest visit's straight-line radius from the depot exceeds the limit."""

    name = "max_route_radius"
    label_en = "Maximum Route Radius"
    description_en = "Penalizes the farthest visit's straight-line distance beyond `Vehicle.max_route_radius_meters`."


class MinimizeEnclosingCircleRadius(Constraint):
    """Radius (meters) of the minimum circle enclosing all visit points. Default weight=0.

    Unlike ``max_route_radius``, which measures from the depot, the depot is not
    one of the circle's points. The geometry behind the enclosing-circle objective.
    """

    name = "minimize_enclosing_circle_radius"
    label_en = "Minimize Enclosing Circle Radius"
    description_en = (
        "Minimizes the smallest enclosing-circle radius of each route's visits. "
        "The default weight is 0, and the depot is not included in the circle."
    )


class MinCapacityUtilization(Constraint):
    """Shortfall of used vehicles below the target minimum load ratio. Default soft."""

    name = "min_capacity_utilization"
    label_en = "Minimum Capacity Utilization"
    description_en = (
        "Penalizes used vehicles whose load ratio is below `Vehicle.min_load_ratio`."
    )


class MinimizeRouteDuration(Constraint):
    """Total route time including waits and service. Default weight=0, so opt-in."""

    name = "minimize_route_duration"
    label_en = "Minimize Route Duration"
    description_en = "Minimizes total route duration including waiting and service (and the return leg only with `return_to_depot`). The default weight is 0."


class MinimizeLatency(Constraint):
    """Sum of the times taken to reach each visit. Default weight=0, so opt-in."""

    name = "minimize_latency"
    label_en = "Minimize Latency"
    description_en = "Minimizes total time from each vehicle's departure to every visit arrival. The default weight is 0."


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
maximize_visits_assigned = MaximizeVisitsAssigned()
max_travel_time = MaxTravelTime()
max_route_duration = MaxRouteDuration()
vehicle_end_time = VehicleEndTime()
max_stops = MaxStops()
max_route_radius = MaxRouteRadius()
minimize_enclosing_circle_radius = MinimizeEnclosingCircleRadius(weight=0)
min_capacity_utilization = MinCapacityUtilization()
minimize_route_duration = MinimizeRouteDuration()
minimize_latency = MinimizeLatency()
vehicle_fixed_cost = VehicleFixedCost()

#: Available set — same order as `vrp.list_constraints()`.
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
    vehicle_fixed_cost,
    maximize_visits_assigned,
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

    ``costs`` is what `vrp.constraint_costs(plan)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = vrp.constraint_costs(plan)
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
