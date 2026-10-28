"""제약 조립 스펙 — 비용 함수를 선언으로 고르는 API.

기본 경로(`Solver(cfg).solve(plan)`)는 PDPTW 핵심 일곱 제약을 켜고, 차량별
route policy 제약은 해당 파라미터가 0이면 비용 0으로 둔다. Duration/latency는
기본 weight 0인 opt-in 목적함수다.
이 모듈은 그 옆의 선택 경로다: 제약을 골라 담고, 가중치를 바꾸고,
하드/소프트를 뒤집은 목록을 `solve_with_constraints` 에 넘긴다.

가장 짧은 사용법은 :func:`get_constraints` 다. 기본 목록을 그대로 돌려주니
"기본에서 조금 바꾸는" 선언이 한 줄로 끝난다::

    import pdp_routing as pdp
    from pdp_routing.constraints import (
        MinimizeTravelTime, get_constraints, vehicle_capacity,
    )

    solver = pdp.Solver(cfg)
    solver.solve_with_constraints(plan, get_constraints())      # 기본 그대로
    solver.solve_with_constraints(plan, get_constraints([       # 골라 담기
        vehicle_capacity, MinimizeTravelTime(weight=10),
    ]))

모듈 상위에 제약마다 기본 인스턴스가 있으니 그중 몇 개만 골라 리스트로
넘겨도 된다. 목록에 넣은 제약만 채점된다 — 빼면 그 항은 점수에서 사라진다.

모든 스펙은 ``(weight=None, hard=None, exponent=1.0)`` 를 받는다:

- ``weight``   제약 비용의 선형 배수. ``None`` 은 컴포넌트 기본값.
- ``hard``     ``True``/``False`` 로 기본 레벨을 뒤집는다; ``None`` 은 유지.
               **이 도메인의 미디엄 제약은 이 손잡이로 표현할 수 없다** —
               ``hard=False`` 는 소프트로 접어 미디엄 의미가 사라진다.
               미디엄 항은 ``None`` 으로 두는 게 맞다.
- ``exponent`` 부호 보존 거듭제곱 셰이핑: ``weight * sign(c) * |c| **
               exponent``. 2.0 = 제곱이라 몰린 위반이 기하급수로 비싸진다.

세 커플링 제약(`paired_request`, `same_vehicle`, `pickup_before_dropoff`)은
서로 겹치지 않게 잘려 있다 — 각 상황을 정확히 한 항이 값매김한다.

이름(`name`)은 C++ 컴포넌트 이름을 snake_case 로 옮긴 것이다. 코어가
기계적으로 유도하니 손으로 관리하는 별칭 표는 없다 —
`pdp.list_constraints()` 로 확인할 수 있다. 모듈 상위 인스턴스 이름도 같은
snake_case 라 `list_constraints()` 가 찍는 문자열과 그대로 대응한다.

플러그인 경로에서는 `parallel_solver_count` 가 적용되지 않는다 (호출자 소유
계산기는 워커로 복제할 수 없다). 짝 인지 move/ruin 연산자는 계산기가 아니라
plan 을 읽으므로 그대로 걸린다.
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
    "VehicleFixedCost",
    "vehicle_capacity",
    "service_finished_after_max_end_time",
    "minimize_travel_time",
    "paired_request",
    "same_vehicle",
    "pickup_before_dropoff",
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
    "vehicle_fixed_cost",
    "get_constraints",
]


@dataclass(frozen=True)
class Constraint:
    """제약 하나를 고르는 스펙: 가중치 / 레벨 / 셰이핑."""

    weight: float | None = None  # None = 컴포넌트 기본 가중치
    hard: bool | None = None  # None = 컴포넌트 기본 레벨
    exponent: float = 1.0
    name: ClassVar[str] = ""
    label_ko: ClassVar[str] = ""
    label_en: ClassVar[str] = ""
    description_ko: ClassVar[str] = ""
    description_en: ClassVar[str] = ""

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} must set a registry `name`")


class VehicleCapacity(Constraint):
    """경로를 따라 쌓인 적재량이 차량 용량을 넘긴 양. 기본: 하드, 가중치 1.

    픽업에서 오르고 대응 드롭오프에서 내리므로 맞춰야 하는 건 경로 총합이
    아니라 그 사이의 **피크**다. 초과분은 그것이 지속되는 모든 노드에서
    부과된다 — full recompute 와 정확히 같다."""

    name = "vehicle_capacity"
    label_ko = "차량 용량"
    label_en = "Vehicle Capacity"
    description_ko = "경로를 따라 쌓인 적재량(픽업에서 오르고 드롭오프에서 내림)이 차량 용량을 넘긴 만큼을 벌점으로 계산합니다. 기본: **하드**, 가중치 1."
    description_en = "Penalizes the running load (up at pickups, down at dropoffs) above the vehicle capacity. Default: **hard**, weight 1."


class ServiceFinishedAfterMaxEndTime(Constraint):
    """노드 서비스가 시간창 끝을 넘긴 분(올림). 기본: 하드, 가중치 1."""

    name = "service_finished_after_max_end_time"
    label_ko = "서비스 종료 시간창 위반"
    label_en = "Service Finished After Max End Time"
    description_ko = "정차 서비스가 시간창 끝을 넘긴 분을 올림해 계산합니다. 기본: **하드**, 가중치 1. **도착 시각 + 서비스 시간**을 기준으로 합니다."
    description_en = "Penalizes rounded-up minutes by which a stop finishes after its time window. Default: **hard**, weight 1. It uses **arrival time + service duration**."


class MinimizeTravelTime(Constraint):
    """차량별 왕복 주행 초 (차고 → 노드들 → 차고). 기본: 소프트, 가중치 1.

    세워 둔 차량은 아무것도 달리지 않으므로 비용이 없다."""

    name = "minimize_travel_time"
    label_ko = "총 주행시간 최소화"
    label_en = "Minimize Travel Time"
    description_ko = "차고 → 정차지들의 차량별 주행시간을 최소화합니다. 복귀 구간은 차량의 `return_to_depot`가 켜진 경우에만 포함됩니다. 기본: **소프트**, 가중치 1."
    description_en = "Minimizes each vehicle's driving time from its home through its stops; the leg back home counts only when the vehicle's `return_to_depot` is on. Default: **soft**, weight 1."


class PairedRequest(Constraint):
    """한쪽 노드만 경로에 실린 요청. 기본: 하드, 가중치 1000.

    배달의 절반은 아예 안 한 것보다 나쁘다 — 실어놓고 안 내려주거나,
    없는 데서 내려주는 꼴이 된다."""

    name = "paired_request"
    label_ko = "요청 쌍 유지"
    label_en = "Paired Request"
    description_ko = "픽업과 드롭오프 중 한쪽만 경로에 실린 요청을 벌점으로 계산합니다. 기본: **하드**, 가중치 1000."
    description_en = "Penalizes requests with only one of pickup and dropoff routed. Default: **hard**, weight 1000."


class SameVehicle(Constraint):
    """픽업과 드롭오프가 **서로 다른** 차량에 실린 요청. 기본: 하드, 가중치 1000.

    중간에 물건을 넘겨주는 사람은 없으므로 두 노드는 같이 다녀야 한다.
    한쪽만 실렸거나 둘 다 안 실린 경우엔 침묵한다 — 그건 `paired_request`
    와 `unassigned_requests` 가 값매김한다."""

    name = "same_vehicle"
    label_ko = "같은 차량"
    label_en = "Same Vehicle"
    description_ko = "픽업과 드롭오프가 서로 다른 차량에 실린 요청을 벌점으로 계산합니다. 기본: **하드**, 가중치 1000."
    description_en = "Penalizes requests whose pickup and dropoff ride on different vehicles. Default: **hard**, weight 1000."


class PickupBeforeDropoff(Constraint):
    """같은 경로에서 드롭오프가 픽업보다 앞선 요청. 기본: 하드, 가중치 1000.

    두 노드가 같은 차량에 있을 때만 본다 — 쪼개진 경우는 `same_vehicle` 이
    값매김한다."""

    name = "pickup_before_dropoff"
    label_ko = "픽업 선행"
    label_en = "Pickup Before Dropoff"
    description_ko = "같은 경로에서 드롭오프가 픽업보다 앞서는 요청을 벌점으로 계산합니다. 기본: **하드**, 가중치 1000."
    description_en = "Penalizes requests whose dropoff precedes its pickup in the same route. Default: **hard**, weight 1000."


class UnassignedRequests(Constraint):
    """두 노드가 **모두** 안 실린 요청의 서비스 분. 기본: 미디엄, 가중치 10^9.

    긴 작업을 버리는 게 짧은 작업을 버리는 것보다 아프다. 반만 실린 요청은
    여기서 안 센다 — 그건 `paired_request` 가 하드 레벨에서 값매김한다.
    미디엄 레벨은 엔진의 소프트 레벨에 `medium * kMediumUnit` 로 접힌다."""

    name = "unassigned_requests"
    label_ko = "미배정 요청 최소화"
    label_en = "Unassigned Requests"
    description_ko = "두 정차지가 모두 실리지 않은 요청의 서비스 분을 벌점으로 계산합니다. 기본: **미디엄**, 가중치 `10^9`."
    description_en = "Penalizes the service minutes of requests left entirely unrouted. Default: **medium**, weight `10^9`."


class MaxTravelTime(Constraint):
    """차량 왕복 주행시간 한도 초과 초."""

    name = "max_travel_time"
    label_ko = "최대 주행시간"
    label_en = "Maximum Travel Time"
    description_ko = "차량의 주행시간(복귀 구간은 `return_to_depot`일 때만 포함)이 `Vehicle.max_travel_time`을 초과한 만큼을 벌점으로 계산합니다."
    description_en = "Penalizes driving time above `Vehicle.max_travel_time` (the leg home counts only with `return_to_depot`)."


class MaxRouteDuration(Constraint):
    """대기·서비스를 포함한 차량 근무시간 한도 초과 초."""

    name = "max_route_duration"
    label_ko = "최대 경로 소요시간"
    label_en = "Maximum Route Duration"
    description_ko = "대기·서비스(복귀는 `return_to_depot`일 때만)를 포함한 경로 소요시간이 `Vehicle.max_route_duration`을 초과한 만큼을 계산합니다."
    description_en = "Penalizes route duration above `Vehicle.max_route_duration`, including waiting and service (and the return leg only with `return_to_depot`)."


class VehicleEndTime(Constraint):
    """차고 복귀 시각이 차량 종료시각을 넘긴 초."""

    name = "vehicle_end_time"
    label_ko = "차량 종료 마감시간"
    label_en = "Vehicle End Time"
    description_ko = "차량의 경로 종료 시각(마지막 정차지 출발, `return_to_depot`이면 차고 복귀)이 `Vehicle.max_end_time`을 초과한 만큼을 벌점으로 계산합니다."
    description_en = "Penalizes the route end time (last-stop departure, or depot return with `return_to_depot`) after `Vehicle.max_end_time`."


class MaxStops(Constraint):
    """차량별 pickup/dropoff 정차 수 한도 초과분."""

    name = "max_stops"
    label_ko = "최대 정차 수"
    label_en = "Maximum Stops"
    description_ko = "차량별 픽업·드롭오프 정차 수가 `Vehicle.max_stops`를 초과한 만큼을 벌점으로 계산합니다(요청 하나가 2개). 기본값 20, 0이면 해제."
    description_en = "Penalizes pickup and dropoff stops above `Vehicle.max_stops` (a request counts as two). Default 20; 0 lifts the cap."


class MaxRouteRadius(Constraint):
    """차고 기준 경로 반경 한도 초과 미터."""

    name = "max_route_radius"
    label_ko = "최대 경로 반경"
    label_en = "Maximum Route Radius"
    description_ko = "차고에서 가장 먼 정차지의 직선거리가 `Vehicle.max_route_radius_meters`를 초과한 만큼을 계산합니다."
    description_en = "Penalizes the farthest stop's straight-line distance beyond `Vehicle.max_route_radius_meters`."


class MinimizeEnclosingCircleRadius(Constraint):
    """픽업·드롭오프 점들을 감싸는 최소 외접원의 반지름(미터).

    depot은 제외하며 기본 weight=0인 opt-in 목적함수다.
    """

    name = "minimize_enclosing_circle_radius"
    label_ko = "최소 외접원 반지름 최소화"
    label_en = "Minimize Enclosing Circle Radius"
    description_ko = "각 경로의 픽업·드롭오프 점을 모두 감싸는 최소 외접원 반지름을 최소화합니다. 기본 가중치는 0이며, 차고는 원의 점에 포함하지 않습니다."
    description_en = "Minimizes the smallest enclosing-circle radius of each route's stops. The default weight is 0, and the home is not included in the circle."


class MinCapacityUtilization(Constraint):
    """사용 차량의 목표 최소 처리량 미달량. 기본 소프트."""

    name = "min_capacity_utilization"
    label_ko = "최소 적재율"
    label_en = "Minimum Capacity Utilization"
    description_ko = "사용 차량의 처리량이 `Vehicle.min_load_ratio` 목표에 미달한 만큼을 벌점으로 계산합니다."
    description_en = "Penalizes used vehicles whose delivered demand is below the `Vehicle.min_load_ratio` target."


class MinimizeRouteDuration(Constraint):
    """대기·서비스 포함 총 경로시간. 기본 weight=0이라 opt-in."""

    name = "minimize_route_duration"
    label_ko = "총 경로 소요시간 최소화"
    label_en = "Minimize Route Duration"
    description_ko = "대기·서비스(복귀는 `return_to_depot`일 때만)를 포함한 전체 경로 소요시간을 최소화합니다. 기본 가중치는 0입니다."
    description_en = "Minimizes total route duration including waiting and service (and the return leg only with `return_to_depot`). The default weight is 0."


class MinimizeLatency(Constraint):
    """각 정차 도착까지 걸린 시간의 합. 기본 weight=0이라 opt-in."""

    name = "minimize_latency"
    label_ko = "총 도착 지연 최소화"
    label_en = "Minimize Latency"
    description_ko = "각 차량의 출발부터 정차지 도착까지 걸린 시간의 합을 최소화합니다. 기본 가중치는 0입니다."
    description_en = "Minimizes total time from each vehicle's departure to every stop arrival. The default weight is 0."


class VehicleFixedCost(Constraint):
    """한 번이라도 쓰인 차량의 고정비."""

    name = "vehicle_fixed_cost"
    label_ko = "차량 고정비"
    label_en = "Vehicle Fixed Cost"
    description_ko = "한 번이라도 사용하는 차량마다 `Vehicle.fixed_cost`를 부과합니다."
    description_en = (
        "Adds `Vehicle.fixed_cost` for every vehicle that is used at least once."
    )


#: 기본 가중치로 만들어 둔 인스턴스. 골라 담을 때 그대로 참조하면 된다.
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
vehicle_fixed_cost = VehicleFixedCost()

#: 사용 가능 세트 — `pdp.list_constraints()` 와 같은 순서.
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
    paired_request,
    same_vehicle,
    pickup_before_dropoff,
    unassigned_requests,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """비용 함수를 돌려준다. 인자가 없으면 등록된 전체 제약.

    `constraints` 를 주면 그 목록을 그대로 쓴다 — 목록에 있는 제약만
    채점된다. 기본에서 하나만 바꾸고 싶으면 기본 인스턴스를 섞어 쓴다::

        get_constraints([vehicle_capacity, MinimizeTravelTime(weight=10)])
    """
    if constraints is None:
        return list(_DEFAULTS)
    if not constraints:
        # C++ applyConstraints 와 같은 계약: 빈 목록은 "아무것도 채점하지
        # 않음" 이라 실수일 가능성이 높다. 필터 결과가 우연히 비었을 때
        # 조용히 "전부 채점" 으로 뒤집히면 안 된다.
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return list(constraints)


class Score(NamedTuple):
    """`score()` 가 돌려주는 (hard, soft). 둘 다 벌점이라 0 이하다."""

    hard: int
    soft: float


def _shape(cost: float, exponent: float) -> float:
    """부호 보존 거듭제곱. 보상(음수 cost)이 벌점으로 뒤집히지 않는다."""
    if exponent == 1.0 or cost == 0.0:
        return cost
    return -((-cost) ** exponent) if cost < 0.0 else cost**exponent


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """제약 원본 cost 에 스펙을 적용해 점수를 낸다 — 순수 파이썬.

    ``costs`` 는 `pdp.constraint_costs(plan)` 가 돌려준 것이다: 제약마다
    unit 별 **원본** cost (가중치도 exponent 도 레벨 반올림도 적용되기 전).
    한 번 받아두면 가중치를 바꿔가며 재채점 없이 계산할 수 있다::

        raw = pdp.constraint_costs(plan)
        score(raw)                     # 기본 세트
        score(raw, picked)             # 골라 담은 목록

    솔버도 탐색 알고리즘도 거치지 않는다. 계산 규칙은 엔진과 같다 —
    unit 마다 ``weight * shape(cost, exponent)`` 를 구하고, 하드는 unit
    기여분을 **각각 반올림**해서 더한다 (엔진의 applyRaw 와 같은 순서;
    합계를 뒤에 반올림하면 값이 달라진다).

    ``constraints`` 를 주면 그 목록의 제약만 채점된다 — `get_constraints`
    와 같은 의미론이다. 모르는 이름은 ValueError.

    돌려주는 값은 **엔진 레벨**의 (hard, soft) 다. 이 도메인은 medium 을
    엔진 soft 에 kMediumUnit (10^9) 배수로 접어 넣으므로, `solution.score` 의 분해된
    레벨과 견주려면 같은 방식으로 접어서 비교해야 한다.
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
                # 엔진과 같이 unit 기여분마다 반올림한다 (llround 와 같은
                # half-away-from-zero; 파이썬 round 는 banker's rounding).
                hard += int(
                    math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5)
                )
            else:
                soft += value
    return Score(-hard, -soft if soft != 0.0 else 0.0)
