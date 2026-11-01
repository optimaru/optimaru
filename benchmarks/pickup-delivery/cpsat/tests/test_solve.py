import pdp_routing as domain
import pytest
from pdp_routing.constraints import (
    PairedRequestCost,
    PickupBeforeDropoffCost,
    SameVehicleCost,
    TravelTimeCost,
    UnassignedRequestsCost,
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
        assert entry["params"] == {"weight": None, "level": None, "exponent": 1.0}
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
