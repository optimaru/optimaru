"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`Solver(cfg).solve(plan)`) enables the seven core PDPTW constraints;
the per-vehicle route-policy constraints cost 0 while their parameter is 0. Route
duration, latency, the enclosing-circle radius and the squared order age are opt-in
objectives with default weight 0.
This module is the alternative path next to it: pick constraints, change weights, change
levels, shape the cost, and pass the list to `solve_with_constraints`.

A constraint list reads one way: one class call per constraint::

    import optimaru.pickup_delivery as pdp
    from optimaru.pickup_delivery.constraints import (
        PairedRequestCost, TravelTimeCost, UnassignedRequestsCost, VehicleCapacityCost,
        get_constraints,
    )

    solver = pdp.Solver(cfg)
    solver.solve_with_constraints(plan, get_constraints())      # the defaults as-is
    solver.solve_with_constraints(plan, [                       # picked
        VehicleCapacityCost(), PairedRequestCost(), UnassignedRequestsCost(),
        TravelTimeCost(weight=10),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``vehicle_capacity_cost``
is ``VehicleCapacityCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost, in units of its level.
               ``None`` = the C++ component default (``pdp.constraint_info()``
               lists them).
- ``level``    ``None`` = the default level, otherwise a ``Level`` or its name
               (``Level.MEDIUM`` or ``"medium"``). Any flippable constraint may
               take any of the three levels. ``UnassignedRequestsCost`` is the one
               **medium** constraint (folded into the engine's soft level as
               ``medium * 10^9``): ``level="hard"`` charges one hard unit per
               minute, ``level="soft"`` one soft unit per minute, and a
               ``weight`` is in units of the level in effect.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``, per vehicle for the route constraints and on the total
               for the four request-level (global) ones. 2.0 = squared, so
               concentrated violations grow much costlier.
- ``offset``   (keyword) shifts that shaping, at the same granularity; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, c - offset) ** exponent``; ``False`` shapes
               ``c - offset`` as it is, so a vehicle below the offset — an idle
               one (cost 0) included — lowers the penalty. The unassigned pool is
               no vehicle: a route constraint never charges it, except
               ``RequiredVehicleCost`` and ``SquaredOrderAgeCost``, which price
               the requests waiting there and so count the pool as a unit. With
               an offset it is refused on hard (``VehicleCapacityCost`` by
               default), because hard 0 must mean no violation.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs and the picks add up. Travel time plus the square of every second
beyond two hours of driving per vehicle::

    solver.solve_with_constraints(plan, [
        *get_constraints(),
        TravelTimeCost(weight=0.001, exponent=2, offset=7200),
    ])

``pdp.constraint_costs(plan, picks)`` keys the second pick ``"travel_time_cost#2"``.

The three coupling constraints (`paired_request_cost`, `same_vehicle_cost`,
`pickup_before_dropoff_cost`) are cut so they never overlap — each situation is
valued by exactly one term.

Names (`name`) are the C++ component names in snake_case
(``VehicleCapacityCost`` -> ``vehicle_capacity_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`pdp.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.

`parallel_solver_count` does not apply on the plugin path (a caller-owned
calculator cannot be cloned into workers). The pair-aware move/ruin operators read
the plan, not the calculator, so they still run.
"""

from .._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "VehicleCapacityCost",
    "MaxEndTimeCost",
    "TravelTimeCost",
    "MaxTravelTimeCost",
    "MaxRouteDurationCost",
    "VehicleMaxEndTimeCost",
    "MaxStopsCost",
    "MaxRouteRadiusCost",
    "EnclosingCircleRadiusCost",
    "MinCapacityUtilizationCost",
    "RouteDurationCost",
    "LatencyCost",
    "SquaredOrderAgeCost",
    "VehicleFixedCost",
    "PinnedStopsCost",
    "RequiredVehicleCost",
    "PairedRequestCost",
    "SameVehicleCost",
    "PickupBeforeDropoffCost",
    "UnassignedRequestsCost",
    "vehicle_capacity_cost",
    "max_end_time_cost",
    "travel_time_cost",
    "max_travel_time_cost",
    "max_route_duration_cost",
    "vehicle_max_end_time_cost",
    "max_stops_cost",
    "max_route_radius_cost",
    "enclosing_circle_radius_cost",
    "min_capacity_utilization_cost",
    "route_duration_cost",
    "latency_cost",
    "squared_order_age_cost",
    "vehicle_fixed_cost",
    "pinned_stops_cost",
    "required_vehicle_cost",
    "paired_request_cost",
    "same_vehicle_cost",
    "pickup_before_dropoff_cost",
    "unassigned_requests_cost",
    "get_constraints",
]


class VehicleCapacityCost(Constraint):
    """Load accumulated along the route above vehicle capacity. Default: hard, weight 1.

    Goods board at the pickup and leave at the matching dropoff, so what must fit
    is the **peak** in between, not the route total. The excess is charged at
    every node where it persists — exactly like a full recompute."""

    name = "vehicle_capacity_cost"
    label_en = "Vehicle capacity"
    label_ko = "차량 용량"
    description_en = (
        "The running load (up at a pickup, down at its dropoff) above the vehicle's capacity, "
        "charged at every stop where the excess persists. One unit of cost is one unit of demand "
        "over capacity at one stop; 0 means no vehicle is ever overloaded."
    )
    description_ko = (
        "픽업에서 늘고 하차에서 줄어드는 적재량이 차량 용량을 넘으면, 초과가 이어지는 정차지마다 그 "
        "초과량을 더합니다. 한 정차지에서 용량을 넘은 수요 1단위가 비용 1이며, 0이면 어떤 차량도 용량을 넘지 않습니다."
    )


class MaxEndTimeCost(Constraint):
    """Minutes (rounded up) by which a stop's service ends after its time window. Default: hard, weight 1.

    It uses **arrival time + service duration** against the stop's `max_end_time`.
    The vehicle's own end of shift is `VehicleMaxEndTimeCost`."""

    name = "max_end_time_cost"
    label_en = "Time window end"
    label_ko = "시간창 마감"
    description_en = (
        "Minutes, rounded up, by which a stop's service (arrival + service duration) finishes after "
        "its time window closes (`max_end_time`). One unit of cost is one late minute at one stop; "
        "0 means every stop finishes in time."
    )
    description_ko = (
        "정차지의 서비스 종료 시각(도착 시각 + 서비스 시간)이 시간창 마감(`max_end_time`)보다 늦은 "
        "시간을 분 단위로 올림해 더합니다. 한 정차지에서 늦은 1분이 비용 1이며, 0이면 모든 정차지의 "
        "서비스가 제시간에 끝납니다."
    )


class TravelTimeCost(Constraint):
    """Driving seconds per vehicle, home -> stops (-> home with return_to_depot). Default: soft, weight 1.

    An idle vehicle drives nothing, so it costs nothing."""

    name = "travel_time_cost"
    label_en = "Travel time"
    label_ko = "운행 시간"
    description_en = (
        "Each vehicle's driving seconds from its home through its stops; the leg back home counts "
        "only when the vehicle's `return_to_depot` is on. One unit of cost is one second of driving; "
        "0 means no vehicle drives."
    )
    description_ko = (
        "차량마다 출발지에서 정차지들을 거쳐 가는 운행 시간(초)을 더합니다. 출발지로 돌아오는 구간은 "
        "`return_to_depot`가 켜져 있을 때만 셉니다. 운행 1초가 비용 1이며, 0이면 아무 차량도 운행하지 않습니다."
    )


class MaxTravelTimeCost(Constraint):
    """Seconds of driving above the vehicle limit. Default: hard, weight 1."""

    name = "max_travel_time_cost"
    label_en = "Maximum travel time"
    label_ko = "최대 운행 시간"
    description_en = (
        "Driving seconds above the vehicle's `max_travel_time` (the leg home counts only with "
        "`return_to_depot`). One unit of cost is one second over the limit; 0 means every vehicle "
        "is within its limit or has none."
    )
    description_ko = (
        "차량의 운행 시간이 `max_travel_time`을 넘는 시간(초)을 더합니다(출발지로 돌아오는 구간은 "
        "`return_to_depot`일 때만 셉니다). 한도를 넘은 1초가 비용 1이며, 0이면 모든 차량이 한도 안에 "
        "있거나 한도가 없습니다."
    )


class MaxRouteDurationCost(Constraint):
    """Seconds of vehicle working time, waits and service included, above the limit. Default: hard, weight 1."""

    name = "max_route_duration_cost"
    label_en = "Maximum route duration"
    label_ko = "최대 경로 소요 시간"
    description_en = (
        "Seconds by which a route's duration (departure to route end, waiting and service included; "
        "the return leg only with `return_to_depot`) exceeds the vehicle's `max_route_duration`. "
        "One unit of cost is one second over the limit; 0 means every route is within its limit or has none."
    )
    description_ko = (
        "출발부터 경로가 끝날 때까지 걸린 시간(대기와 서비스 포함, 귀환 구간은 `return_to_depot`일 때만)이 "
        "차량의 `max_route_duration`을 넘는 시간(초)을 더합니다. 넘은 1초가 비용 1이며, 0이면 모든 경로가 한도 "
        "안에 있거나 한도가 없습니다."
    )


class VehicleMaxEndTimeCost(Constraint):
    """Seconds by which the route end passes the vehicle end time. Default: hard, weight 1."""

    name = "vehicle_max_end_time_cost"
    label_en = "Vehicle end time"
    label_ko = "차량 운행 종료 시각"
    description_en = (
        "Seconds by which a route ends (the last stop's departure, or the return home with "
        "`return_to_depot`) after the vehicle's `max_end_time`. One unit of cost is one second late; "
        "0 means every vehicle finishes in time or has no end time."
    )
    description_ko = (
        "경로가 끝나는 시각(마지막 정차지 출발, `return_to_depot`이면 출발지 도착)이 차량의 "
        "`max_end_time`을 넘는 시간(초)을 더합니다. 늦은 1초가 비용 1이며, 0이면 모든 차량이 제시간에 끝나거나 "
        "종료 시각이 없습니다."
    )


class MaxStopsCost(Constraint):
    """Pickup/dropoff stops per vehicle above the limit. Default: hard, weight 1."""

    name = "max_stops_cost"
    label_en = "Maximum stops"
    label_ko = "최대 정차 수"
    description_en = (
        "Pickup and dropoff stops above the vehicle's `max_stops` (a request counts as two). One unit "
        "of cost is one stop over the cap; 0 means every vehicle is within its cap. The JSON model "
        "defaults `max_stops` to 20; 0 lifts the cap."
    )
    description_ko = (
        "차량의 픽업·하차 정차 수가 `max_stops`를 넘는 만큼을 더합니다(요청 하나는 정차 두 번). 넘은 정차 "
        "1회가 비용 1이며, 0이면 모든 차량이 한도 안에 있습니다. JSON 모델의 `max_stops` 기본값은 20이고, "
        "0이면 한도가 없습니다."
    )


class MaxRouteRadiusCost(Constraint):
    """Meters of route radius from the vehicle's home above the limit. Default: hard, weight 1."""

    name = "max_route_radius_cost"
    label_en = "Maximum route radius"
    label_ko = "최대 경로 반경"
    description_en = (
        "Meters by which the farthest stop's straight-line distance from the vehicle's home exceeds "
        "`max_route_radius_meters`. One unit of cost is one meter beyond the radius; 0 means every "
        "stop is inside it or no radius is set."
    )
    description_ko = (
        "출발지에서 가장 먼 정차지까지의 직선거리가 `max_route_radius_meters`를 넘는 거리(미터)를 더합니다. "
        "반경을 벗어난 1미터가 비용 1이며, 0이면 모든 정차지가 반경 안에 있거나 반경이 설정되지 않았습니다."
    )


class EnclosingCircleRadiusCost(Constraint):
    """Radius (meters) of the minimum enclosing circle of a route's stops. Default: soft, weight 0 (opt-in).

    The home is excluded; give it a weight to prefer compact routes."""

    name = "enclosing_circle_radius_cost"
    label_en = "Enclosing circle radius"
    label_ko = "경로 최소 포함 원 반지름"
    description_en = (
        "Radius in meters of the smallest circle enclosing each route's stops (home excluded), to "
        "prefer compact routes. One unit of cost is one meter; 0 for a route with fewer than two stops. "
        "Opt-in: the default weight is 0."
    )
    description_ko = (
        "경로마다 정차지(출발지 제외)를 모두 담는 가장 작은 원의 반지름(미터)을 더해, 좁은 범위에 모인 "
        "경로를 선호하게 합니다. 1미터가 비용 1이며, 정차지가 둘 미만인 경로는 0입니다. 기본 가중치가 "
        "0이므로 가중치를 주어야 점수에 반영됩니다."
    )


class MinCapacityUtilizationCost(Constraint):
    """Shortfall of used vehicles below the target minimum load. Default: soft, weight 1."""

    name = "min_capacity_utilization_cost"
    label_en = "Minimum capacity utilization"
    label_ko = "최소 적재율"
    description_en = (
        "For each used vehicle, how far its delivered demand (the sum of its pickups) falls short of "
        "`capacity * min_load_ratio`. One unit of cost is one unit of demand short; 0 means every used "
        "vehicle reaches its target or has none."
    )
    description_ko = (
        "운행하는 차량마다 실어 나른 수요(픽업 수요의 합)가 `capacity * min_load_ratio` 목표에 못 미치는 "
        "양을 더합니다. 모자란 수요 1단위가 비용 1이며, 0이면 운행하는 모든 차량이 목표를 채우거나 목표가 "
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
        "`return_to_depot`일 때만)을 더합니다. 1초가 비용 1이며, 운행하는 차량이 없으면 0입니다. 기본 "
        "가중치가 0이므로 가중치를 주어야 점수에 반영됩니다."
    )


class LatencyCost(Constraint):
    """Sum of the times taken to reach each stop. Default: soft, weight 0 (opt-in)."""

    name = "latency_cost"
    label_en = "Latency"
    label_ko = "누적 도착 시간"
    description_en = (
        "The time from each vehicle's departure to its arrival at every stop, summed over the stops. "
        "One unit of cost is one second; 0 means no stop is served. Opt-in: the default weight is 0."
    )
    description_ko = (
        "차량이 출발한 뒤 각 정차지에 도착하기까지 걸린 시간을 모든 정차지에 대해 더합니다. 1초가 비용 "
        "1이며, 방문하는 정차지가 없으면 0입니다. 기본 가중치가 0이므로 가중치를 주어야 점수에 반영됩니다."
    )


class SquaredOrderAgeCost(Constraint):
    """Squared seconds from each order to its dropoff finishing. Default: soft, weight 0 (opt-in).

    Only dropoffs count, and each request counts on its own, so one very late
    order costs more than several slightly late ones. An unassigned request is
    charged as if finished an hour into the plan: otherwise leaving out the
    oldest orders would be the cheapest thing to do. It is an order *age*, not
    lateness past a due time. The square is part of the raw cost, so an
    exponent compounds it (exponent 2 gives a 4th power)."""

    name = "squared_order_age_cost"
    label_en = "Squared order age"
    label_ko = "주문 경과 시간 제곱"
    description_en = (
        "For every request, the squared seconds from its `order_time` to the end of its dropoff, so "
        "one very old order costs more than several slightly old ones. An unassigned request is charged "
        "as if its dropoff finished one hour into the plan. One unit of cost is one second squared; 0 "
        "only when every dropoff finishes at its order time. Opt-in: the default weight is 0."
    )
    description_ko = (
        "요청마다 `order_time`부터 하차가 끝날 때까지 걸린 시간(초)의 제곱을 더하므로, 아주 오래 기다린 "
        "주문 하나가 조금씩 기다린 주문 여럿보다 비용이 큽니다. 배정되지 않은 요청은 계획 시작 1시간 뒤에 하차가 끝난 "
        "것으로 계산합니다. 1초의 제곱이 비용 1이며, 모든 하차가 주문 시각에 끝날 때만 0입니다. 기본 "
        "가중치가 0이므로 가중치를 주어야 점수에 반영됩니다."
    )


class VehicleFixedCost(Constraint):
    """Fixed cost of every vehicle used at least once. Default: soft, weight 1."""

    name = "vehicle_fixed_cost"
    label_en = "Vehicle fixed cost"
    label_ko = "차량 고정비"
    description_en = (
        "Adds `Vehicle.fixed_cost` for every vehicle that serves at least one stop. One unit of cost "
        "is one unit of fixed cost; 0 means no vehicle with a fixed cost is used."
    )
    description_ko = (
        "정차지를 하나라도 맡은 차량마다 `Vehicle.fixed_cost`를 더합니다. 고정비 1단위가 비용 1이며, "
        "0이면 고정비가 있는 차량을 하나도 쓰지 않습니다."
    )


class PinnedStopsCost(Constraint):
    """Routes that do not start with their vehicle's pinned stops. Default: hard, weight 1e12.

    Each misplaced stop counts once. Silent for a vehicle that pins nothing. The weight is far above
    what the other hard rules total on real input, so no trade between them moves a pinned stop."""

    name = "pinned_stops_cost"
    label_en = "Pinned stops"
    label_ko = "고정 정차지"
    description_en = (
        "Positions at the start of a route that do not hold the vehicle's pinned stops, in order. One "
        "unit of cost is one misplaced pinned stop; 0 means every route starts with its pinned stops. "
        "The weight 1e12 keeps any trade between other hard rules from moving a pinned stop."
    )
    description_ko = (
        "경로 앞부분에서 차량의 고정 정차지가 정해진 순서대로 놓이지 않은 자리를 셉니다. 어긋난 고정 "
        "정차지 하나가 비용 1이며, 0이면 모든 경로가 고정 정차지로 시작합니다. 가중치가 1e12로 매우 커서, "
        "다른 하드 규칙의 위반을 줄이려고 고정 정차지를 옮기는 일이 생기지 않습니다."
    )


class RequiredVehicleCost(Constraint):
    """Requests off the vehicle they must ride on. Default: hard, weight 1000.

    Unassigned counts once and another vehicle twice, so a request with no place on its vehicle waits.
    Where on the route the request goes stays free."""

    name = "required_vehicle_cost"
    label_en = "Required vehicle"
    label_ko = "지정 차량"
    description_en = (
        "Requests that are not on their `required_vehicle`: 1 while unassigned, 2 on another vehicle, "
        "so a request with no room on its vehicle waits rather than rides the wrong one. 0 means every "
        "bound request rides its vehicle."
    )
    description_ko = (
        "지정 차량(`required_vehicle`)에 배정되지 않은 요청을 셉니다. 미배정이면 1, 다른 차량에 배정되면 "
        "2이므로, 지정 차량에 자리가 없는 요청은 다른 차량에 배정되지 않고 미배정으로 남습니다. 0이면 "
        "차량이 지정된 요청이 모두 그 차량에 배정됩니다."
    )


class PairedRequestCost(Constraint):
    """Requests with only one node on a route. Default: hard, weight 1000.

    Half a delivery is worse than none — goods get loaded and never dropped off,
    or dropped off from nowhere. Structural on the CP-SAT reference (hard only)."""

    name = "paired_request_cost"
    label_en = "Paired request"
    label_ko = "요청 짝 맞춤"
    description_en = (
        "Requests with only one of their pickup and dropoff routed. One unit of cost is one "
        "half-routed request; 0 means every request is routed whole or not at all."
    )
    description_ko = (
        "픽업과 하차 가운데 하나만 경로에 들어간 요청을 셉니다. 반쪽만 배정된 요청 하나가 비용 1이며, "
        "0이면 모든 요청의 픽업과 하차가 함께 배정되거나 함께 빠져 있습니다."
    )


class SameVehicleCost(Constraint):
    """Requests whose pickup and dropoff ride on **different** vehicles. Default: hard, weight 1000.

    Nobody hands goods over midway, so both nodes must travel together.
    Silent when only one or neither is assigned — `paired_request_cost` and
    `unassigned_requests_cost` value those. Structural on the CP-SAT reference (hard only)."""

    name = "same_vehicle_cost"
    label_en = "Same vehicle"
    label_ko = "같은 차량"
    description_en = (
        "Requests whose pickup and dropoff ride on different vehicles. One unit of cost is one split "
        "request; 0 means every routed request stays on one vehicle."
    )
    description_ko = (
        "픽업과 하차가 서로 다른 차량에 배정된 요청을 셉니다. 나뉜 요청 하나가 비용 1이며, 0이면 배정된 "
        "모든 요청의 픽업과 하차가 같은 차량에 있습니다."
    )


class PickupBeforeDropoffCost(Constraint):
    """Requests whose dropoff precedes the pickup on the same route. Default: hard, weight 1000.

    Only looks at both nodes on the same vehicle — split cases are valued by
    `same_vehicle_cost`. Structural on the CP-SAT reference (hard only)."""

    name = "pickup_before_dropoff_cost"
    label_en = "Pickup before dropoff"
    label_ko = "픽업 후 하차"
    description_en = (
        "Requests whose dropoff comes before their pickup on the same route. One unit of cost is one "
        "reversed request; 0 means every pickup precedes its dropoff."
    )
    description_ko = (
        "같은 경로에서 하차가 픽업보다 앞에 놓인 요청을 셉니다. 순서가 뒤바뀐 요청 하나가 비용 1이며, "
        "0이면 모든 픽업이 하차보다 먼저입니다."
    )


class UnassignedRequestsCost(Constraint):
    """Service minutes of requests with **neither** node assigned. Default: medium, weight 1.

    Dropping a long job hurts more than dropping a short one. Half-assigned
    requests are not counted here — `paired_request_cost` values them at the hard
    level. The weight is in medium units; the domain folds the medium level into
    the engine soft level as `medium * 10^9`."""

    name = "unassigned_requests_cost"
    label_en = "Unassigned requests"
    label_ko = "미배정 요청"
    description_en = (
        "Service minutes (pickup plus dropoff, rounded down) of the requests left entirely unrouted, "
        "on the medium level. One unit of cost is one minute; 0 means no request is left out. A "
        "half-routed request is counted by `paired_request_cost` instead."
    )
    description_ko = (
        "경로에 전혀 들어가지 않은 요청의 서비스 시간(픽업과 하차 합, 분 단위 내림)을 미디엄 수준에서 "
        "더합니다. 1분이 비용 1이며, 0이면 빠진 요청이 없습니다. 반쪽만 배정된 요청은 "
        "`paired_request_cost`가 셉니다."
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
squared_order_age_cost = SquaredOrderAgeCost()
vehicle_fixed_cost = VehicleFixedCost()
pinned_stops_cost = PinnedStopsCost()
required_vehicle_cost = RequiredVehicleCost()
paired_request_cost = PairedRequestCost()
same_vehicle_cost = SameVehicleCost()
pickup_before_dropoff_cost = PickupBeforeDropoffCost()
unassigned_requests_cost = UnassignedRequestsCost()

# The default set, in list_constraints() order (unit constraints, then the four globals).
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
    squared_order_age_cost,
    vehicle_fixed_cost,
    pinned_stops_cost,
    required_vehicle_cost,
    paired_request_cost,
    same_vehicle_cost,
    pickup_before_dropoff_cost,
    unassigned_requests_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, every registered constraint.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([VehicleCapacityCost(), UnassignedRequestsCost(), TravelTimeCost(weight=10)])

    Same contract as the C++ applyConstraints: an empty list is an error (pass
    None for the defaults); a constraint named several times is scored once per
    pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `pdp.constraint_costs(plan)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent, offset and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = pdp.constraint_costs(plan)
        score(raw)                     # default set
        score(raw, [VehicleCapacityCost(), UnassignedRequestsCost(), TravelTimeCost(weight=10)])
        score(raw, [TravelTimeCost(), TravelTimeCost(exponent=2, offset=3600)])

    No solver and no search are involved. The arithmetic matches the engine —
    each evaluated unit (a vehicle, the pool too for the two constraints that
    price it; a global's total) contributes ``weight * shape(cost, exponent,
    offset, clip)`` and hard contributions are **rounded per unit** before
    summing (the same order as the engine's applyRaw; rounding the total
    afterwards gives different values). A second pick of a constraint reads the
    same raw costs with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft): the medium level
    (``unassigned_requests_cost``) is folded into soft as ``medium * 10^9``, the
    ``medium_fold`` every entry of ``costs`` carries. To compare with the split
    levels of `plan.score`, fold the same way first.
    """
    return score_costs(costs, get_constraints(constraints))
