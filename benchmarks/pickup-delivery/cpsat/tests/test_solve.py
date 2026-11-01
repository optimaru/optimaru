from itertools import permutations, product

import pdp_routing as domain
import pytest
from ortools.sat.python import cp_model
from pdp_routing.constraints import (
    MaxEndTimeCost,
    MaxStopsCost,
    PairedRequestCost,
    PickupBeforeDropoffCost,
    SameVehicleCost,
    TravelTimeCost,
    UnassignedRequestsCost,
    VehicleCapacityCost,
    paired_request_cost,
    pickup_before_dropoff_cost,
    same_vehicle_cost,
    score,
    vehicle_capacity_cost,
)
from pdp_routing.models import (
    DeliveryModel,
    PdpInput,
    PdpRequest,
    StopModel,
    VehicleModel,
    to_plan,
)
from pdp_routing.models import (
    solve_request as solve_metaheuristic_request,
)
from pickup_delivery_cpsat import catalog, solve_request
from pickup_delivery_cpsat.constraints import REGISTRY

# Engine constraints the CP-SAT reference has no plugin for.
ENGINE_ONLY = {
    "enclosing_circle_radius_cost",
    "squared_order_age_cost",
    "pinned_stops_cost",
    "required_vehicle_cost",
}
MEDIUM_UNIT = 1_000_000_000  # the engine's medium fold (into soft)


def test_catalog_exposes_route_policies() -> None:
    rows = catalog()
    assert all(row["label_en"] for row in rows)
    assert all(row["description_en"] for row in rows)
    assert {row["name"] for row in rows} == {
        "vehicle_capacity_cost",
        "max_end_time_cost",
        "max_travel_time_cost",
        "max_route_duration_cost",
        "vehicle_max_end_time_cost",
        "max_stops_cost",
        "max_route_radius_cost",
        "unassigned_requests_cost",
        "min_capacity_utilization_cost",
        "travel_time_cost",
        "route_duration_cost",
        "latency_cost",
        "vehicle_fixed_cost",
        "paired_request_cost",
        "same_vehicle_cost",
        "pickup_before_dropoff_cost",
    }


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """Every plugin reads its defaults off the engine's constraint_info() row."""
    engine = {row.name: row for row in domain.constraint_info()}
    entries = catalog()
    # list_constraints() order, minus the engine-only constraints.
    assert [entry["name"] for entry in entries] == [
        name for name in domain.list_constraints() if name not in ENGINE_ONLY
    ]
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert (
            entry["kind"] == "either"
        )  # every pickup-delivery constraint is flippable
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }
    assert engine["unassigned_requests_cost"].default_level == "medium"
    assert (
        next(e for e in entries if e["name"] == "unassigned_requests_cost")[
            "default_weight"
        ]
        == 1
    )


def test_pickup_precedes_dropoff() -> None:
    request = PdpRequest(
        timelimit=2,
        model_input=PdpInput(
            vehicles=[VehicleModel(id="v", capacity=2, home=(37.0, 127.0))],
            deliveries=[
                DeliveryModel(
                    id="d",
                    demand=2,
                    pickup=StopModel(
                        id="p", location=(37.01, 127.0), service_duration=60
                    ),
                    dropoff=StopModel(
                        id="q", location=(37.02, 127.0), service_duration=60
                    ),
                )
            ],
        ),
    )

    solved = solve_request(request)

    assert solved.feasible
    assert [stop.pickup for stop in solved.routes["v"]] == [True, False]

    meta = solve_metaheuristic_request(
        request.model_copy(update={"timelimit": 0.1}),
        constraints=[vehicle_capacity_cost],
    )
    assert set(meta.routes) == {"v"}
    assert meta.score


def test_max_stops_can_leave_a_delivery_unassigned() -> None:
    request = PdpRequest(
        timelimit=2,
        model_input=PdpInput(
            vehicles=[
                VehicleModel(id="v", capacity=10, home=(37.0, 127.0), max_stops=2)
            ],
            deliveries=[
                DeliveryModel(
                    id=f"d{index}",
                    demand=1,
                    pickup=StopModel(
                        id=f"p{index}",
                        location=(37.01 + index * 0.01, 127.0),
                        service_duration=60,
                    ),
                    dropoff=StopModel(
                        id=f"q{index}",
                        location=(37.02 + index * 0.01, 127.0),
                        service_duration=60,
                    ),
                )
                for index in range(2)
            ],
        ),
    )

    solved = solve_request(request)

    assert solved.feasible
    assert len(solved.routes["v"]) == 2
    assert len(solved.unassigned) == 1


def _two_requests(timelimit: float = 5) -> PdpRequest:
    return PdpRequest(
        timelimit=timelimit,
        model_input=PdpInput(
            vehicles=[VehicleModel(id="v", capacity=10, home=(37.0, 127.0))],
            deliveries=[
                DeliveryModel(
                    id=f"d{index}",
                    demand=1,
                    pickup=StopModel(
                        id=f"p{index}",
                        location=(37.01 + index * 0.01, 127.0),
                        service_duration=300,
                    ),
                    dropoff=StopModel(
                        id=f"q{index}",
                        location=(37.02 + index * 0.01, 127.0),
                        service_duration=300,
                    ),
                )
                for index in range(2)
            ],
        ),
    )


def _engine_score(request: PdpRequest, routes, selection):
    """The engine's (hard, soft) for the routes CP-SAT chose, under the same selection."""
    plan = to_plan(request.model_input)
    node_by_id = {node.id: i for i, node in enumerate(plan.nodes)}
    for vehicle in plan.vehicles:
        vehicle.node_indices = [node_by_id[stop.stop_id] for stop in routes[vehicle.id]]
    plan.rebuild_unassigned_pool()
    return score(domain.constraint_costs(plan), selection)


def _levels(value: str) -> dict[str, int]:
    parsed = {}
    for part in value.split("/"):
        for level in ("hard", "medium", "soft"):
            if part.endswith(level):
                parsed[level] = int(part[: -len(level)])
    return parsed


def test_demoting_the_medium_constraint_scores_like_the_engine() -> None:
    """level="soft" moves unassigned_requests_cost to soft on BOTH solvers.

    With driving hard, parking the vehicle wins: both requests are dropped and
    their 2 x 10 service minutes land on soft, not on medium."""
    selection = [
        paired_request_cost,
        same_vehicle_cost,
        pickup_before_dropoff_cost,
        UnassignedRequestsCost(level="soft"),
        TravelTimeCost(level="hard"),
    ]
    request = _two_requests()
    solved = solve_request(request, constraints=selection)
    levels = _levels(solved.score)
    assert levels == {"hard": 0, "medium": 0, "soft": -20}
    assert sorted(solved.unassigned) == ["d0", "d1"]
    engine = _engine_score(request, solved.routes, selection)
    assert (engine.hard, engine.soft) == (
        levels["hard"],
        levels["soft"] + levels["medium"] * MEDIUM_UNIT,
    )


def test_default_selection_scores_like_the_engine() -> None:
    """At the defaults (medium unassigned_requests_cost), CP-SAT routes both requests and the
    engine scores its routes the same, the medium level folded into soft."""
    request = _two_requests()
    solved = solve_request(request)
    levels = _levels(solved.score)
    assert levels["hard"] == 0 and levels["medium"] == 0
    cpsat_names = [entry["name"] for entry in catalog()]
    selection = [
        spec
        for spec in domain.constraints.get_constraints()
        if spec.name in cpsat_names
    ]
    engine = _engine_score(request, solved.routes, selection)
    assert (engine.hard, engine.soft) == (
        levels["hard"],
        levels["soft"] + levels["medium"] * MEDIUM_UNIT,
    )


@pytest.mark.parametrize(
    "spec_class", [PairedRequestCost, SameVehicleCost, PickupBeforeDropoffCost]
)
@pytest.mark.parametrize(
    "knobs",
    [{"level": "soft"}, {"weight": 0}, {"weight": 1}],
    ids=["soft", "weight-0", "weight-1"],
)
def test_structural_constraints_refuse_what_the_model_cannot_honour(
    spec_class, knobs
) -> None:
    """The circuit model enforces the coupling rules; it cannot price a violation."""
    with pytest.raises(ValueError, match="enforces it structurally"):
        solve_request(_two_requests(timelimit=1), constraints=[spec_class(**knobs)])


@pytest.mark.parametrize(
    "spec_class", [PairedRequestCost, SameVehicleCost, PickupBeforeDropoffCost]
)
def test_structural_constraints_accept_their_engine_default(spec_class) -> None:
    for pick in (spec_class(), spec_class(level="hard"), spec_class(weight=1000)):
        solved = solve_request(_two_requests(timelimit=1), constraints=[pick])
        assert _levels(solved.score)["hard"] == 0


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------

# Everything on a line, the position in seconds of driving from the depots.
LINE = {"V1": 0, "V2": 0, "P0": 100, "D0": 300, "P1": 200, "D1": 500}


def _line_request(timelimit: float = 5) -> PdpRequest:
    """Two vehicles (V1 carries 3), two requests: r0 (demand 4, its dropoff due at 250 s)
    and r1 (demand 2, 300 s of service each, 10 penalty minutes when left out)."""
    order = list(LINE)
    return PdpRequest(
        timelimit=timelimit,
        model_input=PdpInput(
            vehicles=[
                VehicleModel(id="V1", capacity=3, home=(37.0, 127.0)),
                VehicleModel(id="V2", capacity=10, home=(37.0, 127.0)),
            ],
            deliveries=[
                DeliveryModel(
                    id="r0",
                    demand=4,
                    pickup=StopModel(
                        id="P0", location=(37.0, 127.0), service_duration=60
                    ),
                    dropoff=StopModel(
                        id="D0",
                        location=(37.0, 127.0),
                        service_duration=60,
                        max_end_time=250,
                    ),
                ),
                DeliveryModel(
                    id="r1",
                    demand=2,
                    pickup=StopModel(
                        id="P1", location=(37.0, 127.0), service_duration=300
                    ),
                    dropoff=StopModel(
                        id="D1", location=(37.0, 127.0), service_duration=300
                    ),
                ),
            ],
            driving_seconds=[[abs(LINE[a] - LINE[b]) for b in order] for a in order],
        ),
    )


def _route_sets():
    """Every solution of the line instance: each request on V1, V2 or none, the stops of
    a vehicle with both requests in each of the six pickup-first interleavings."""
    pairs = {"r0": ("P0", "D0"), "r1": ("P1", "D1")}
    for where in product((None, "V1", "V2"), repeat=2):
        routes = {"V1": [], "V2": []}
        for request, vehicle in zip(pairs, where):
            if vehicle:
                routes[vehicle].append(request)
        choices = []
        for vehicle, requests in routes.items():
            stops = [stop for request in requests for stop in pairs[request]]
            orders = {
                order
                for order in permutations(stops)
                if all(
                    order.index(pairs[r][0]) < order.index(pairs[r][1])
                    for r in requests
                )
            }
            choices.append([(vehicle, order) for order in sorted(orders)])
        for combination in product(*choices):
            yield dict(combination)


def _engine_levels(request, routes, selection):
    """The engine's (hard, folded soft) for `routes` under `selection`, and the plan's costs."""
    plan = to_plan(request.model_input)
    node_by_id = {node.id: i for i, node in enumerate(plan.nodes)}
    for vehicle in plan.vehicles:
        vehicle.node_indices = [node_by_id[stop_id] for stop_id in routes[vehicle.id]]
    plan.rebuild_unassigned_pool()
    engine = score(domain.constraint_costs(plan, selection), selection)
    return (engine.hard, engine.soft), domain.constraint_costs(plan)


SELECTIONS = {
    "repeat": [
        VehicleCapacityCost(),
        TravelTimeCost(),
        TravelTimeCost(weight=2, offset=100),
    ],
    "repeat-literal": [
        UnassignedRequestsCost(),
        UnassignedRequestsCost(weight=2, offset=3, clip=False),
        TravelTimeCost(offset=150, clip=False),
        TravelTimeCost(weight=3, offset=-20),
        MaxStopsCost(offset=1, clip=False),  # 0 on both vehicles: -1 each, not the pool
    ],
    "hard-offsets": [
        VehicleCapacityCost(offset=1),
        VehicleCapacityCost(weight=2, offset=2, clip=False),
        MaxEndTimeCost(offset=1, clip=False),
        PairedRequestCost(offset=1, clip=False),
        UnassignedRequestsCost(),
    ],
    "identical-twice": [TravelTimeCost(), TravelTimeCost(), UnassignedRequestsCost()],
}


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_every_solution(selection) -> None:
    """Pin each of the 19 solutions: the CP-SAT objective's levels are the engine's score
    of those routes under the same repeated / shifted picks, exactly."""
    request = _line_request(timelimit=5)
    for routes in _route_sets():
        solved = solve_request(request, constraints=selection, _fixed_routes=routes)
        levels = _levels(solved.score)
        engine, costs = _engine_levels(request, routes, selection)
        assert (
            levels["hard"],
            levels["soft"] + levels["medium"] * MEDIUM_UNIT,
        ) == engine, routes
        # Re-pricing the default costs (each name once) gives the same per instance key.
        objective = REGISTRY.objective(
            cp_model.CpModel(), ("hard", "medium", "soft"), selection
        )
        assert objective.score_costs(costs) == levels, routes


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_reach_the_brute_force_optimum(selection) -> None:
    request = _line_request()
    best = max(
        _engine_levels(request, routes, selection)[0] for routes in _route_sets()
    )
    solved = solve_request(request, constraints=selection)
    levels = _levels(solved.score)
    assert (levels["hard"], levels["soft"] + levels["medium"] * MEDIUM_UNIT) == best


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, offset, clip} dicts pick the same instances as specs."""
    specs = [
        UnassignedRequestsCost(),
        TravelTimeCost(),
        TravelTimeCost(weight=3, offset=100, clip=False),
    ]
    dicts = [
        {"name": "unassigned_requests_cost"},
        {"name": "travel_time_cost"},
        {"name": "travel_time_cost", "weight": 3, "offset": 100, "clip": False},
    ]
    assert solve_request(_line_request(), constraints=dicts) == solve_request(
        _line_request(), constraints=specs
    )


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve_request(
            _line_request(timelimit=1), constraints=[TravelTimeCost(offset=0.5)]
        )


def test_a_repeated_structural_pick_refuses_per_instance() -> None:
    """Each instance of a structural rule is checked on its own: the second pick's
    level="soft" is refused even though the first one is the default."""
    with pytest.raises(ValueError, match="enforces it structurally"):
        solve_request(
            _two_requests(timelimit=1),
            constraints=[PairedRequestCost(), PairedRequestCost(level="soft")],
        )
    solved = solve_request(
        _two_requests(timelimit=1),
        constraints=[PairedRequestCost(), PairedRequestCost()],
    )
    assert _levels(solved.score)["hard"] == 0
