"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`Solver(cfg).solve(plan)`) enables the four core constraints;
the per-vehicle route-policy constraints cost 0 while their parameter is 0. The
enclosing-circle radius, route duration and latency are opt-in objectives with
default weight 0.
This module is the alternative path next to it: pick constraints, change weights, change
levels, shape the cost, and pass the list to `solve_with_constraints`.

A constraint list reads one way: one class call per constraint::

    import optimaru.vehicle_routing as vrp
    from optimaru.vehicle_routing.constraints import (
        TravelTimeCost, UnassignedVisitsCost, VehicleCapacityCost, get_constraints,
    )

    solver = vrp.Solver(cfg)
    solver.solve_with_constraints(plan, get_constraints())      # the defaults as-is
    solver.solve_with_constraints(plan, [                       # picked
        VehicleCapacityCost(), UnassignedVisitsCost(), TravelTimeCost(weight=10),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``vehicle_capacity_cost``
is ``VehicleCapacityCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost, in units of its level.
               ``None`` = the C++ component default (``vrp.constraint_info()``
               lists them).
- ``level``    ``None`` = the default level, otherwise a ``Level`` or its name
               (``Level.MEDIUM`` or ``"medium"``). Any flippable constraint may
               take any of the three levels. ``UnassignedVisitsCost`` is the one
               **medium** constraint (folded into the engine's soft level as
               ``medium * 10^9``): ``level="hard"`` charges one hard unit per
               penalty minute, ``level="soft"`` one soft unit per penalty minute,
               and a ``weight`` is in units of the level in effect.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``, per vehicle for the route constraints and on the total
               for the global ``UnassignedVisitsCost``. 2.0 = squared, so
               concentrated violations grow much costlier.
- ``offset``   (keyword) shifts that shaping, at the same granularity; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a vehicle below the offset — an idle
               one (cost 0) included — lowers the penalty. The unassigned pool is
               no vehicle: a route constraint never charges it. With an offset it
               is refused on hard (``VehicleCapacityCost`` by default), because
               hard 0 must mean no violation.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs and the picks add up. Travel time plus the cube of every second beyond
two hours of driving per vehicle::

    solver.solve_with_constraints(plan, [
        VehicleCapacityCost(),
        UnassignedVisitsCost(),
        TravelTimeCost(),
        TravelTimeCost(weight=1e-9, exponent=3, offset=7200),
    ])

``vrp.constraint_costs(plan, picks)`` keys the second pick ``"travel_time_cost#2"``.

Names (`name`) are the C++ component names in snake_case
(``VehicleCapacityCost`` -> ``vehicle_capacity_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`vrp.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.

`parallel_solver_count` applies on the plugin path: every start scores on its own copy
of the picks (in C++, only the `Solver::solve` overload that takes one caller-owned
calculator stays a single start). The driving-time-guided selection does not apply there.
Details are in the comments of `cpp/include/vrp/solver.hpp`.
"""

from .._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "EnclosingCircleRadiusCost",
    "LatencyCost",
    "Level",
    "MaxEndTimeCost",
    "MaxRouteDurationCost",
    "MaxRouteRadiusCost",
    "MaxStopsCost",
    "MaxTravelTimeCost",
    "MinCapacityUtilizationCost",
    "RouteDurationCost",
    "Score",
    "TravelTimeCost",
    "UnassignedVisitsCost",
    "VehicleCapacityCost",
    "VehicleFixedCost",
    "VehicleMaxEndTimeCost",
    "VisitGroupCost",
    "VisitOrderCost",
    "enclosing_circle_radius_cost",
    "get_constraints",
    "latency_cost",
    "max_end_time_cost",
    "max_route_duration_cost",
    "max_route_radius_cost",
    "max_stops_cost",
    "max_travel_time_cost",
    "min_capacity_utilization_cost",
    "route_duration_cost",
    "score",
    "travel_time_cost",
    "unassigned_visits_cost",
    "vehicle_capacity_cost",
    "vehicle_fixed_cost",
    "vehicle_max_end_time_cost",
    "visit_group_cost",
    "visit_order_cost",
]


class VehicleCapacityCost(Constraint):
    """Demand loaded beyond what the vehicle can carry. Default: hard, weight 1."""

    name = "vehicle_capacity_cost"
    label_en = "Vehicle capacity"
    label_ko = "차량 용량"
    description_en = (
        "The summed demand of a vehicle's visits above its capacity. One unit of cost is one unit "
        "of demand over capacity; 0 means no vehicle is overloaded."
    )
    description_ko = (
        "차량이 맡은 방문지의 수요 합이 차량 용량을 넘는 만큼을 더합니다. 용량을 넘은 수요 1단위가 "
        "비용 1이며, 0이면 어떤 차량도 용량을 넘지 않습니다."
    )


class MaxEndTimeCost(Constraint):
    """Minutes (rounded up) by which a visit's service ends after its time window. Default: hard, weight 1.

    Measured from **arrival** + service duration, not service start. The vehicle's own end time is `VehicleMaxEndTimeCost`."""

    name = "max_end_time_cost"
    label_en = "Time window end"
    label_ko = "시간창 마감"
    description_en = (
        "Minutes, rounded up, by which a visit's service (arrival + service duration) finishes after "
        "its time window closes (`max_end_time`). One unit of cost is one late minute at one visit; "
        "0 means every visit finishes in time."
    )
    description_ko = (
        "방문지의 서비스 종료 시각(도착 시각 + 서비스 시간)이 시간창 마감(`max_end_time`)을 넘은 시간을 "
        "분 단위로 올림해 더합니다. 한 방문지에서 늦은 1분이 비용 1이며, 0이면 모든 방문지가 제시간에 "
        "끝납니다."
    )


class TravelTimeCost(Constraint):
    """Driving seconds per vehicle, home -> visits (-> home with return_to_depot). Default: soft, weight 1.

    An idle vehicle drives nothing, so it costs nothing."""

    name = "travel_time_cost"
    label_en = "Travel time"
    label_ko = "운행 시간"
    description_en = (
        "Each vehicle's driving seconds from its home through its visits; the leg back home counts "
        "only when the vehicle's `return_to_depot` is on. One unit of cost is one second of driving; "
        "0 means no vehicle drives."
    )
    description_ko = (
        "차량마다 출발지에서 방문지들을 거쳐 가는 운행 시간(초)을 더합니다. 출발지로 돌아오는 구간은 "
        "`return_to_depot`가 켜져 있을 때만 셉니다. 운행 1초가 비용 1이며, 0이면 아무 차량도 운행하지 "
        "않습니다."
    )


class MaxTravelTimeCost(Constraint):
    """Seconds of driving above ``Vehicle.max_travel_time``. Default: hard, weight 1."""

    name = "max_travel_time_cost"
    label_en = "Maximum travel time"
    label_ko = "최대 운행 시간"
    description_en = (
        "Driving seconds above the vehicle's `max_travel_time` (the leg home counts only with "
        "`return_to_depot`). One unit of cost is one second over the limit; 0 means every vehicle "
        "is within its limit or has none."
    )
    description_ko = (
        "차량의 운행 시간이 `max_travel_time`을 넘은 시간(초)을 더합니다. 출발지로 돌아오는 구간은 "
        "`return_to_depot`가 켜져 있을 때만 셉니다. 한도를 넘은 1초가 비용 1이며, 0이면 모든 차량이 "
        "한도 안에 있거나 한도가 없습니다."
    )


class MaxRouteDurationCost(Constraint):
    """Seconds of working time, waits and service included, above the limit. Default: hard, weight 1."""

    name = "max_route_duration_cost"
    label_en = "Maximum route duration"
    label_ko = "최대 경로 소요 시간"
    description_en = (
        "Seconds by which a route's duration (departure to route end, waiting and service included; "
        "the return leg only with `return_to_depot`) exceeds the vehicle's `max_route_duration`. "
        "One unit of cost is one second over the limit; 0 means every route is within its limit or has none."
    )
    description_ko = (
        "출발부터 경로가 끝날 때까지 걸린 시간(대기와 서비스 포함, 귀환 구간은 `return_to_depot`가 켜져 "
        "있을 때만)이 차량의 `max_route_duration`을 넘은 시간(초)을 더합니다. 한도를 넘은 1초가 비용 "
        "1이며, 0이면 모든 경로가 한도 안에 있거나 한도가 없습니다."
    )


class VehicleMaxEndTimeCost(Constraint):
    """Seconds by which the route end passes ``Vehicle.max_end_time``. Default: hard, weight 1.

    A visit's own time window is `MaxEndTimeCost`."""

    name = "vehicle_max_end_time_cost"
    label_en = "Vehicle end time"
    label_ko = "차량 운행 종료 시각"
    description_en = (
        "Seconds by which a route ends (the last visit's departure, or the return home with "
        "`return_to_depot`) after the vehicle's `max_end_time`. One unit of cost is one second late; "
        "0 means every vehicle finishes in time or has no end time."
    )
    description_ko = (
        "경로가 끝나는 시각(마지막 방문지를 떠나는 시각, `return_to_depot`가 켜져 있으면 출발지에 "
        "돌아온 시각)이 차량의 `max_end_time`을 넘은 시간(초)을 더합니다. 늦은 1초가 비용 1이며, 0이면 "
        "모든 차량이 제시간에 끝나거나 종료 시각 제한이 없습니다."
    )


class MaxStopsCost(Constraint):
    """Visits per vehicle above ``Vehicle.max_stops``. Default: hard, weight 1."""

    name = "max_stops_cost"
    label_en = "Maximum stops"
    label_ko = "최대 방문 수"
    description_en = (
        "Visits on a route above the vehicle's `max_stops`. One unit of cost is one visit over the "
        "cap; 0 means every vehicle is within its cap or has none."
    )
    description_ko = (
        "차량이 맡은 방문지 수가 `max_stops`를 넘는 만큼을 더합니다. 넘은 방문 1회가 비용 1이며, 0이면 "
        "모든 차량이 한도 안에 있거나 한도가 없습니다."
    )


class MaxRouteRadiusCost(Constraint):
    """Meters of route radius from the vehicle's home above the limit. Default: hard, weight 1."""

    name = "max_route_radius_cost"
    label_en = "Maximum route radius"
    label_ko = "최대 경로 반경"
    description_en = (
        "Meters by which the farthest visit's straight-line distance from the vehicle's home exceeds "
        "`max_route_radius_meters`. One unit of cost is one meter beyond the radius; 0 means every "
        "visit is inside it or no radius is set."
    )
    description_ko = (
        "출발지에서 가장 먼 방문지까지의 직선거리가 `max_route_radius_meters`를 넘은 거리(미터)를 더합니다. "
        "반경을 벗어난 1미터가 비용 1이며, 0이면 모든 방문지가 반경 안에 있거나 반경이 설정되지 않았습니다."
    )


class EnclosingCircleRadiusCost(Constraint):
    """Radius (meters) of the minimum circle enclosing a route's visits. Default: soft, weight 0 (opt-in).

    Unlike ``max_route_radius_cost``, which measures from the depot, the depot is
    not one of the circle's points. Give it a weight to prefer compact routes."""

    name = "enclosing_circle_radius_cost"
    label_en = "Enclosing circle radius"
    label_ko = "경로 최소 포함 원 반지름"
    description_en = (
        "Radius in meters of the smallest circle enclosing each route's visits (the depot excluded), "
        "to prefer compact routes. One unit of cost is one meter; 0 for a route with fewer than two "
        "visits. Opt-in: the default weight is 0."
    )
    description_ko = (
        "경로마다 방문지(출발지 제외)를 모두 담는 가장 작은 원의 반지름(미터)을 더해, 방문지가 좁은 "
        "범위에 모인 경로를 선호하게 합니다. 1미터가 비용 1이며, 방문지가 둘 미만인 경로는 0입니다. 기본 "
        "가중치가 0이므로 쓰려면 가중치를 직접 지정해야 합니다."
    )


class MinCapacityUtilizationCost(Constraint):
    """Shortfall of used vehicles below the target minimum load. Default: soft, weight 1."""

    name = "min_capacity_utilization_cost"
    label_en = "Minimum capacity utilization"
    label_ko = "최소 적재율"
    description_en = (
        "For each used vehicle, how far the summed demand of its visits falls short of "
        "`capacity * min_load_ratio`. One unit of cost is one unit of demand short; 0 means every used "
        "vehicle reaches its target or has none."
    )
    description_ko = (
        "운행하는 차량마다 맡은 방문지의 수요 합이 `capacity * min_load_ratio` 목표에 못 미치는 양을 "
        "더합니다. 모자란 수요 1단위가 비용 1이며, 0이면 운행하는 모든 차량이 목표를 채우거나 목표가 "
        "없습니다."
    )


class RouteDurationCost(Constraint):
    """Total route time including waits and service. Default: soft, weight 0 (opt-in)."""

    name = "route_duration_cost"
    label_en = "Route duration"
    label_ko = "경로 소요 시간"
    description_en = (
        "Each used vehicle's working time from departure to route end, waiting and service included "
        "(the return leg only with `return_to_depot`). One unit of cost is one second; 0 means no "
        "vehicle is used. Opt-in: the default weight is 0."
    )
    description_ko = (
        "운행하는 차량마다 출발부터 경로가 끝날 때까지의 시간(대기와 서비스 포함, 귀환 구간은 "
        "`return_to_depot`가 켜져 있을 때만)을 더합니다. 1초가 비용 1이며, 운행하는 차량이 없으면 0입니다. 기본 "
        "가중치가 0이므로 쓰려면 가중치를 직접 지정해야 합니다."
    )


class LatencyCost(Constraint):
    """Sum of the times taken to reach each visit. Default: soft, weight 0 (opt-in)."""

    name = "latency_cost"
    label_en = "Latency"
    label_ko = "누적 도착 시간"
    description_en = (
        "The time from each vehicle's departure to its arrival at every visit, summed over the visits. "
        "One unit of cost is one second; 0 means no visit is served. Opt-in: the default weight is 0."
    )
    description_ko = (
        "차량이 출발한 뒤 각 방문지에 도착하기까지 걸린 시간을 모든 방문지에 대해 더합니다. 1초가 비용 "
        "1이며, 방문하는 곳이 없으면 0입니다. 기본 가중치가 0이므로 쓰려면 가중치를 직접 지정해야 "
        "합니다."
    )


class VehicleFixedCost(Constraint):
    """Fixed cost of every vehicle used at least once. Default: soft, weight 1."""

    name = "vehicle_fixed_cost"
    label_en = "Vehicle fixed cost"
    label_ko = "차량 고정비"
    description_en = (
        "Adds `Vehicle.fixed_cost` for every vehicle that serves at least one visit. One unit of cost "
        "is one unit of fixed cost; 0 means no vehicle with a fixed cost is used."
    )
    description_ko = (
        "방문지를 하나라도 맡은 차량마다 `Vehicle.fixed_cost`를 더합니다. 고정비 1단위가 비용 1이며, "
        "0이면 고정비가 있는 차량을 하나도 쓰지 않습니다."
    )


class VisitOrderCost(Constraint):
    """Visits served before one they have to come after. Default: hard, weight 1000.

    `after` on a visit names visits of its group that are served earlier on the route:
    a partial order on the group. Engine-only (no CP-SAT plugin)."""

    name = "visit_order_cost"
    label_en = "Visit order"
    label_ko = "방문 순서"
    description_en = (
        "Pairs of visits on one route that are served the wrong way round: a visit before one that "
        "its `after` says it follows. One unit of cost is one such pair; 0 means every route keeps "
        "the order. A pair whose visits ride different vehicles is priced by `visit_group_cost`."
    )
    description_ko = (
        "한 경로에서 순서가 뒤바뀐 방문지 쌍을 셉니다. `after`에 적힌 방문지보다 먼저 방문한 경우입니다. "
        "뒤바뀐 쌍 하나가 비용 1이며, 0이면 모든 경로가 순서를 지킵니다. 두 방문지가 서로 다른 차량에 "
        "있는 경우는 `visit_group_cost`가 셉니다."
    )


class VisitGroupCost(Constraint):
    """Grouped visits that are apart from the rest of their group. Default: hard, weight 1000.

    The visits of a `group` are one job: they ride one vehicle together, or all wait.
    A global constraint, since a group spans routes. Engine-only (no CP-SAT plugin)."""

    name = "visit_group_cost"
    label_en = "Visit group"
    label_ko = "방문지 묶음"
    description_en = (
        "Visits that are not where most of their `group` is: on another vehicle, or left unassigned "
        "while the others ride (or riding while the others wait). One unit of cost is one such visit; "
        "0 means every group rides one vehicle together or waits together."
    )
    description_ko = (
        "같은 묶음(`group`)의 방문지 가운데 묶음의 다수와 떨어져 있는 방문지를 셉니다. 다른 차량에 "
        "배정됐거나, 나머지는 배정됐는데 혼자 미배정이거나 그 반대인 방문지 하나가 비용 1이며, 0이면 모든 "
        "묶음이 한 차량에 함께 배정되거나 함께 미배정으로 남습니다."
    )


class UnassignedVisitsCost(Constraint):
    """Penalty minutes of visits left off every route. Default: medium, weight 1.

    The per-visit penalty defaults to the service time in whole minutes
    (`Visit.unassigned_penalty`); it is penalty minutes, not a count. Unassigned
    visits belong to no unit, so this is a global constraint. The weight is in
    medium units; the domain folds the medium level into the engine soft level
    as `medium * 10^9`."""

    name = "unassigned_visits_cost"
    label_en = "Unassigned visits"
    label_ko = "미배정 방문지"
    description_en = (
        "The unassigned penalty (`unassigned_penalty_minutes`, by default the service time in whole "
        "minutes) of every visit left off all routes, on the medium level. One unit of cost is one "
        "penalty minute; 0 means no visit with a penalty is left out."
    )
    description_ko = (
        "어느 경로에도 들어가지 않은 방문지마다 미배정 페널티(`unassigned_penalty_minutes`, 기본값은 "
        "서비스 시간을 분 단위로 내림한 값)를 미디엄 수준에서 더합니다. 페널티 1분이 비용 1이며, "
        "0이면 페널티가 있는 방문지가 하나도 빠지지 않았습니다."
    )


#: Ready-made default instances (``vehicle_capacity_cost`` is ``VehicleCapacityCost()``).
vehicle_capacity_cost = VehicleCapacityCost()
max_end_time_cost = MaxEndTimeCost()
travel_time_cost = TravelTimeCost()
max_travel_time_cost = MaxTravelTimeCost()
max_route_duration_cost = MaxRouteDurationCost()
vehicle_max_end_time_cost = VehicleMaxEndTimeCost()
max_stops_cost = MaxStopsCost()
max_route_radius_cost = MaxRouteRadiusCost()
enclosing_circle_radius_cost = EnclosingCircleRadiusCost()
min_capacity_utilization_cost = MinCapacityUtilizationCost()
route_duration_cost = RouteDurationCost()
latency_cost = LatencyCost()
vehicle_fixed_cost = VehicleFixedCost()
visit_order_cost = VisitOrderCost()
visit_group_cost = VisitGroupCost()
unassigned_visits_cost = UnassignedVisitsCost()

# The default set, in list_constraints() order (unit constraints, then the two globals).
_DEFAULTS = (
    vehicle_capacity_cost,
    max_end_time_cost,
    travel_time_cost,
    max_travel_time_cost,
    max_route_duration_cost,
    vehicle_max_end_time_cost,
    max_stops_cost,
    max_route_radius_cost,
    enclosing_circle_radius_cost,
    min_capacity_utilization_cost,
    route_duration_cost,
    latency_cost,
    vehicle_fixed_cost,
    visit_order_cost,
    visit_group_cost,
    unassigned_visits_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, every registered constraint.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([VehicleCapacityCost(), UnassignedVisitsCost(), TravelTimeCost(weight=10)])

    Same contract as the C++ applyConstraints: an empty list is an error (pass
    None for the defaults); a constraint named several times is scored once per
    pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `vrp.constraint_costs(plan)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent, offset and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = vrp.constraint_costs(plan)
        score(raw)                     # default set
        score(raw, [VehicleCapacityCost(), UnassignedVisitsCost(), TravelTimeCost(weight=10)])
        score(raw, [TravelTimeCost(), TravelTimeCost(exponent=2, offset=3600)])

    No solver and no search are involved. The arithmetic matches the engine —
    each evaluated unit (a vehicle; the global's total) contributes ``weight *
    shape(cost, exponent, offset, clip)`` and hard contributions are **rounded
    per unit** before summing (the same order as the engine's applyRaw;
    rounding the total afterwards gives different values). A second pick of a
    constraint reads the same raw costs with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft): the medium level
    (``unassigned_visits_cost``) is folded into soft as ``medium * 10^9``, the
    ``medium_fold`` every entry of ``costs`` carries. To compare with the split
    levels of `plan.score`, fold the same way first.
    """
    return score_costs(costs, get_constraints(constraints))
