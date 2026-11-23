"""Profile requests: the requests a vehicle's profile may not carry, and those it is preferred for."""

import pytest

import optimaru.pickup_delivery as pdp
from optimaru.pickup_delivery.constraints import (
    PairedRequestCost,
    ProfilePreferredRequestCost,
    TravelTimeCost,
    UnassignedRequestsCost,
    VehicleCapacityCost,
    score,
)
from optimaru.pickup_delivery.models import (
    DeliveryModel,
    DepotModel,
    PdpInput,
    PdpRequest,
    StopModel,
    TravelProfile,
    VehicleModel,
    solve_request,
    to_plan,
)


def stop(name: str) -> StopModel:
    return StopModel(id=name, location=(37.5, 127.0), max_end_time=10**7, service_duration=60)


def request(name: str, group: str | None = None, required_vehicle: str | None = None) -> DeliveryModel:
    return DeliveryModel(
        id=name,
        demand=1,
        group=group,
        required_vehicle=required_vehicle,
        pickup=stop(f"{name}p"),
        dropoff=stop(f"{name}d"),
    )


def vehicle(name: str, profile: str, route=(), pinned: int = 0) -> VehicleModel:
    return VehicleModel(
        id=name,
        capacity=5,
        start_depot=name,
        profile=profile,
        max_stops=0,
        initial_route=list(route),
        pinned_stops=pinned,
    )


# `a` and `b` are by vehicle v, `c` is by vehicle w.
PLACES = {"v": 0, "w": 10_000, "ap": 10, "ad": 20, "bp": 30, "bd": 40, "cp": 10_010, "cd": 10_020}


def line_input(lists: dict[str, dict], deliveries=None, vehicles=None, places=PLACES) -> PdpInput:
    """Vehicles v (profile "basic") and w (profile "skilled") on one shared road profile."""
    vehicles = vehicles or [vehicle("v", "basic"), vehicle("w", "skilled")]
    deliveries = deliveries or [request("a"), request("b"), request("c")]
    order = [v.start_depot for v in vehicles]
    for delivery in deliveries:
        order += [delivery.pickup.id, delivery.dropoff.id]
    road = TravelProfile(durations={x: {y: abs(places[x] - places[y]) for y in order} for x in order})
    profiles = {"road": road} | {
        name: TravelProfile(same_travel_as="road", **lists.get(name, {})) for name in ("basic", "skilled")
    }
    return PdpInput(
        depots=[DepotModel(id=v.start_depot, location=(37.5, 127.0)) for v in vehicles],
        vehicles=vehicles,
        deliveries=deliveries,
        profiles=profiles,
    )


def solve(model_input: PdpInput, timelimit: float = 0.3, constraints=None):
    return solve_request(PdpRequest(model_input=model_input, timelimit=timelimit), constraints=constraints)


def vehicle_of(solution) -> dict[str, str]:
    return {stop.delivery_id: name for name, route in solution.routes.items() for stop in route}


def routed(model_input: PdpInput, routes: dict[str, list[str]]) -> "pdp.PdpRoutePlan":
    plan = to_plan(model_input)
    index = {node.id: i for i, node in enumerate(plan.nodes)}
    for vehicle_ in plan.vehicles:
        vehicle_.node_indices = [index[stop_id] for stop_id in routes.get(vehicle_.id, [])]
    plan.vehicles = plan.vehicles  # the list read above is a copy: write it back
    plan.rebuild_unassigned_pool()
    return plan


# Costs per list: the vehicles v and w, then the unassigned pool (always 0).
def costs(model_input: PdpInput, name: str, routes: dict[str, list[str]]) -> list[float]:
    return pdp.constraint_costs(routed(model_input, routes))[name]["costs"]


def test_lists_name_known_requests_once_and_never_both():
    with pytest.raises(ValueError, match="unknown requests \\['x'\\]"):
        line_input({"basic": {"forbidden_requests": ["x"]}})
    with pytest.raises(ValueError, match="preferred_requests names \\['a'\\] twice"):
        TravelProfile(preferred_requests=["a", "a"])
    with pytest.raises(ValueError, match="both in forbidden_requests and in preferred_requests"):
        TravelProfile(forbidden_requests=["a"], preferred_requests=["a"])
    with pytest.raises(ValueError, match="must ride vehicle v, whose profile forbids it"):
        line_input({"basic": {"forbidden_requests": ["a"]}}, deliveries=[request("a", required_vehicle="v")])
    with pytest.raises(ValueError, match="pins a stop of request a, which its profile forbids"):
        line_input(
            {"basic": {"forbidden_requests": ["a"]}},
            deliveries=[request("a")],
            vehicles=[vehicle("v", "basic", route=["ap", "ad"], pinned=1), vehicle("w", "skilled")],
        )


def test_to_plan_gives_the_profiles_entries_and_one_shared_matrix():
    plan = to_plan(line_input({"basic": {"forbidden_requests": ["c", "a"]}, "skilled": {"preferred_requests": ["b"]}}))
    assert plan.profile_count == 1
    assert [v.profile_requests_index for v in plan.vehicles] == [0, 1]
    entries = [(e.forbidden_request_indices, e.preferred_request_indices) for e in plan.profile_requests]
    assert entries == [([0, 2], []), ([], [1])]


def test_the_compiled_plan_refuses_bad_profile_requests():
    plan = to_plan(line_input({}))
    with pytest.raises(ValueError, match="strictly increasing"):
        pdp.PdpRoutePlan(
            "bad",
            plan.locations,
            plan.vehicles,
            plan.nodes,
            plan.deliveries,
            profile_requests=[pdp.ProfileRequests([1, 0])],
        )


def test_the_forbidden_cost_counts_each_stop_of_a_forbidden_request():
    model_input = line_input({"basic": {"forbidden_requests": ["a"]}})
    name = "profile_forbidden_request_cost"
    assert costs(model_input, name, {"v": ["ap", "ad", "bp", "bd"]}) == [2, 0, 0]
    assert costs(model_input, name, {"v": ["ap"], "w": ["ad"]}) == [1, 0, 0]
    assert costs(model_input, name, {"w": ["ap", "ad"]}) == [0, 0, 0]


def test_the_preferred_cost_counts_each_stop_off_its_preferring_profile():
    model_input = line_input({"skilled": {"preferred_requests": ["a"]}})
    name = "profile_preferred_request_cost"
    assert costs(model_input, name, {"v": ["ap", "ad"]}) == [2, 0, 0]
    assert costs(model_input, name, {"w": ["ap", "ad"], "v": ["bp", "bd"]}) == [0, 0, 0]


@pytest.mark.parametrize("timelimit", [0, 0.3])
def test_a_forbidden_request_goes_to_a_vehicle_that_may_carry_it(timelimit):
    solution = solve(line_input({"basic": {"forbidden_requests": ["a"]}}), timelimit)

    assert solution.feasible and not solution.unassigned
    assert vehicle_of(solution)["a"] == "w"


@pytest.mark.parametrize("timelimit", [0, 0.3])
def test_a_request_no_vehicle_may_carry_stays_unassigned(timelimit):
    model_input = line_input({"basic": {"forbidden_requests": ["a"]}, "skilled": {"forbidden_requests": ["a"]}})

    solution = solve(model_input, timelimit)

    assert solution.feasible and solution.unassigned == ["a"]


def test_a_group_goes_to_a_vehicle_that_may_carry_all_of_it():
    model_input = line_input(
        {"basic": {"forbidden_requests": ["b"]}}, deliveries=[request("a", "g"), request("b", "g"), request("c")]
    )
    for timelimit in (0, 0.3):
        solution = solve(model_input, timelimit)
        assert solution.feasible and not solution.unassigned
        assert vehicle_of(solution)["a"] == vehicle_of(solution)["b"] == "w"


def test_forbidden_requests_hold_whether_the_constraints_list_their_cost_or_not():
    model_input = line_input({"basic": {"forbidden_requests": ["a"]}})
    picks = [VehicleCapacityCost(), UnassignedRequestsCost(), TravelTimeCost(), PairedRequestCost()]

    solution = solve(model_input, constraints=picks)

    assert vehicle_of(solution)["a"] == "w"
    raw = pdp.constraint_costs(routed(model_input, {"v": ["ap", "ad"]}), picks)
    assert "profile_forbidden_request_cost" in raw
    assert score(raw, picks).hard == -2000


def test_preferences_are_honored_when_they_cost_little_driving():
    places = {"v": 0, "w": 30, "ap": 12, "ad": 14, "bp": 40, "bd": 45, "cp": 50, "cd": 55}
    assert vehicle_of(solve(line_input({}, places=places)))["a"] == "v"

    solution = solve(line_input({"skilled": {"preferred_requests": ["a"]}}, places=places))

    assert solution.feasible and vehicle_of(solution)["a"] == "w"


def test_preferences_give_way_to_a_long_drive():
    solution = solve(line_input({"skilled": {"preferred_requests": ["a"]}}))

    assert vehicle_of(solution)["a"] == "v"


def test_preferred_requests_need_their_cost_among_the_constraints():
    model_input = line_input({"skilled": {"preferred_requests": ["a"]}})
    picks = [VehicleCapacityCost(), UnassignedRequestsCost(), TravelTimeCost(), PairedRequestCost()]

    with pytest.raises(ValueError, match="leave out profile_preferred_request_cost"):
        solve(model_input, constraints=picks)
    solve(model_input, constraints=[*picks, ProfilePreferredRequestCost(weight=0)])


def test_calculate_score_refuses_a_profile_requests_index_out_of_range():
    plan = to_plan(line_input({}))
    vehicles = plan.vehicles
    vehicles[0].profile_requests_index = 3
    plan.vehicles = vehicles
    with pytest.raises(ValueError, match="profileRequestsIndex 3 is out of range"):
        pdp.calculate_score(plan)


def test_the_compiled_plan_refuses_a_pinned_stop_its_profile_forbids():
    plan = to_plan(line_input({}))
    vehicles = plan.vehicles
    vehicles[0].profile_requests_index = 0
    vehicles[0].pinned_node_indices = [0]  # the pickup of request a
    with pytest.raises(ValueError, match="pins a stop of request a, which its profile forbids"):
        pdp.PdpRoutePlan(
            "bad", plan.locations, vehicles, plan.nodes, plan.deliveries, profile_requests=[pdp.ProfileRequests([0])]
        )


def test_same_travel_as_shares_the_matrix_of_the_named_profile():
    plan = to_plan(line_input({"basic": {"forbidden_requests": ["a"]}}))
    assert plan.profile_count == 1
    assert [v.profile_index for v in plan.vehicles] == [0, 0]
    assert plan.driving_time(0, 2, 0) == PLACES["ap"]  # the road profile's seconds, depot v to ap


@pytest.mark.parametrize("timelimit", [0, 0.3])
def test_a_group_with_a_request_no_vehicle_may_carry_waits_whole(timelimit):
    # b may ride neither vehicle, so its group with a waits; c is free.
    model_input = line_input(
        {"basic": {"forbidden_requests": ["b"]}, "skilled": {"forbidden_requests": ["b"]}},
        deliveries=[request("a", "g"), request("b", "g"), request("c")],
    )

    solution = solve(model_input, timelimit)

    assert solution.feasible
    assert sorted(solution.unassigned) == ["a", "b"]
    assert "c" in vehicle_of(solution)
