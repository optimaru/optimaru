"""Profile visits: the visits a vehicle's profile may not serve, and those it is preferred for."""

import pytest

import optimaru.vehicle_routing as vrp
from optimaru.vehicle_routing.constraints import (
    ProfileForbiddenVisitCost,
    ProfilePreferredVisitCost,
    TravelTimeCost,
    UnassignedVisitsCost,
    VehicleCapacityCost,
    get_constraints,
    score,
)
from optimaru.vehicle_routing.models import (
    DepotModel,
    RoutePlanRequest,
    RoutingInput,
    TravelProfile,
    VehicleModel,
    VisitModel,
    solve_request,
    to_plan,
)

from .._picks import without


def visit(name: str, group: str | None = None) -> VisitModel:
    return VisitModel(id=name, location=(37.5, 127.0), max_end_time=10**7, service_duration=600, group=group)


def vehicle(name: str, profile: str, route=(), **fields) -> VehicleModel:
    """A vehicle that starts at a depot of its own, named like it."""
    return VehicleModel(id=name, capacity=10, start_depot=name, profile=profile, initial_route=list(route), **fields)


# `a` and `b` are by vehicle v, `c` is by vehicle w.
PLACES = {"v": 0, "w": 10_000, "a": 10, "b": 20, "c": 10_010}


def line_input(lists: dict[str, dict], visits=None, vehicles=None, places=PLACES) -> RoutingInput:
    """Vehicles v (profile "basic") and w (profile "skilled"), everything on a line.

    `lists` gives each profile its visit lists; "road" holds the travel the two share."""
    vehicles = vehicles or [vehicle("v", "basic"), vehicle("w", "skilled")]
    visits = visits or [visit("a"), visit("b"), visit("c")]
    order = [v.start_depot for v in vehicles] + [v.id for v in visits]
    road = TravelProfile(durations={x: {y: abs(places[x] - places[y]) for y in order} for x in order})
    profiles = {"road": road} | {
        name: TravelProfile(same_travel_as="road", **lists.get(name, {})) for name in ("basic", "skilled")
    }
    return RoutingInput(
        depots=[DepotModel(id=v.start_depot, location=(37.5, 127.0)) for v in vehicles],
        vehicles=vehicles,
        visits=visits,
        profiles=profiles,
    )


def solve(model_input: RoutingInput, timelimit: float = 0.3, constraints=None, seed: int = 0):
    request = RoutePlanRequest(model_input=model_input, timelimit=timelimit, random_seed=seed)
    return solve_request(request, constraints=constraints)


def vehicle_of(solution) -> dict[str, str]:
    return {stop.visit_id: name for name, route in solution.routes.items() for stop in route}


# Costs per list: the vehicles v and w, then the unassigned pool (always 0).
def costs(model_input: RoutingInput, name: str, routes: dict[str, list[str]], picks=None) -> list[float]:
    plan = to_plan(model_input)
    index = {v.id: i for i, v in enumerate(plan.visits)}
    for vehicle_ in plan.vehicles:
        vehicle_.visit_indices = [index[visit_id] for visit_id in routes.get(vehicle_.id, [])]
    plan.vehicles = plan.vehicles  # the list read above is a copy: write it back
    plan.rebuild_unassigned_pool()
    return vrp.constraint_costs(plan, picks)[name]["costs"]


# --- the input ---------------------------------------------------------------


def test_lists_name_known_visits_once_and_never_both():
    with pytest.raises(ValueError, match="unknown visits \\['x'\\]"):
        line_input({"basic": {"forbidden_visits": ["x"]}})
    with pytest.raises(ValueError, match="unknown visits \\['y'\\]"):
        line_input({"skilled": {"preferred_visits": ["a", "y"]}})
    with pytest.raises(ValueError, match="forbidden_visits names \\['a'\\] twice"):
        TravelProfile(forbidden_visits=["a", "a"])
    with pytest.raises(ValueError, match="both in forbidden_visits and in preferred_visits"):
        TravelProfile(forbidden_visits=["a", "b"], preferred_visits=["b"])


def test_same_travel_as_names_another_profile_that_gives_its_own_travel():
    with pytest.raises(ValueError, match="leave out durations"):
        TravelProfile(same_travel_as="road", durations={"a": {"b": 1}})
    with pytest.raises(ValueError, match="leave out missing_value"):
        TravelProfile(same_travel_as="road", missing_value=5)
    base = line_input({})
    for profiles, message in (
        ({"basic": TravelProfile(same_travel_as="nope")}, "unknown profile 'nope'"),
        ({"basic": TravelProfile(same_travel_as="basic")}, "names the profile itself"),
        (
            {"basic": TravelProfile(same_travel_as="skilled"), "skilled": TravelProfile(same_travel_as="road")},
            "which drives as 'road'",
        ),
    ):
        data = base.model_dump()
        data["profiles"] |= {name: profile.model_dump() for name, profile in profiles.items()}
        with pytest.raises(ValueError, match=message):
            RoutingInput.model_validate(data)


def test_profiles_that_share_travel_share_one_matrix():
    model_input = line_input({"basic": {"forbidden_visits": ["c"]}, "skilled": {"preferred_visits": ["a"]}})
    plan = to_plan(model_input)
    # road, basic and skilled drive alike: one matrix, and two profile visit entries.
    assert plan.profile_count == 1
    assert [v.profile_index for v in plan.vehicles] == [0, 0]
    assert [v.profile_visits_index for v in plan.vehicles] == [0, 1]
    entries = [(e.forbidden_visit_indices, e.preferred_visit_indices) for e in plan.profile_visits]
    assert entries == [([2], []), ([], [0])]
    # A profile that gives no travel and names none drives straight lines, one copy for all such.
    straight = RoutingInput(
        depots=[DepotModel(id="D", location=(37.5, 127.0))],
        vehicles=[VehicleModel(id=x, capacity=1, start_depot="D", profile=x) for x in ("p", "q")],
        visits=[visit("a")],
        profiles={"p": TravelProfile(forbidden_visits=["a"]), "q": TravelProfile()},
    )
    assert to_plan(straight).profile_count == 1
    # A profile driving as one on straight lines shares that matrix too.
    data = straight.model_dump()
    data["vehicles"].append({"id": "r", "capacity": 1, "start_depot": "D", "profile": "r"})
    data["profiles"]["r"] = {"same_travel_as": "q", "preferred_visits": ["a"]}
    plan = to_plan(RoutingInput.model_validate(data))
    assert plan.profile_count == 1
    assert [v.profile_index for v in plan.vehicles] == [0, 0, 0]


def test_profiles_that_drive_alike_share_one_matrix_and_the_others_keep_theirs():
    data = line_input({}).model_dump()
    order = ["v", "w", "a", "b", "c"]
    data["profiles"] = {
        "line1": {},
        "seconds": {"durations": {x: {y: 7 for y in order} for x in order}},
        "line2": {"forbidden_visits": ["a"]},
        "like_seconds": {"same_travel_as": "seconds"},
    }
    data["vehicles"][0]["profile"] = "line2"
    data["vehicles"][1]["profile"] = "like_seconds"
    plan = to_plan(RoutingInput.model_validate(data))
    # line1 and line2 drive straight lines (one matrix), seconds its own, like_seconds as seconds.
    assert plan.profile_count == 2
    assert [v.profile_index for v in plan.vehicles] == [0, 1]
    assert plan.driving_time(0, 2, 1) == 7


def test_profiles_without_lists_give_no_entries():
    plan = to_plan(line_input({}))
    assert plan.profile_visits == []
    assert [v.profile_visits_index for v in plan.vehicles] == [-1, -1]


def test_the_compiled_plan_refuses_bad_profile_visits():
    locations = [vrp.Location(0, 0), vrp.Location(0, 1)]
    visits = [vrp.Visit("C", "C", 1, 1, 0, 3600, 0), vrp.Visit("E", "E", 1, 1, 0, 3600, 0)]

    def plan(entries, index=0):
        vehicle_ = vrp.Vehicle("V", 1, 0, profile_visits_index=index)
        return vrp.VehicleRoutePlan("bad", locations, [vehicle_], visits, profile_visits=entries)

    with pytest.raises(ValueError, match="profileVisitsIndex 1 is out of range"):
        plan([vrp.ProfileVisits([0])], index=1)
    with pytest.raises(ValueError, match="out of range 0..1"):
        plan([vrp.ProfileVisits([2])])
    with pytest.raises(ValueError, match="strictly increasing"):
        plan([vrp.ProfileVisits([1, 0])])
    with pytest.raises(ValueError, match="visit E is both forbidden and preferred"):
        plan([vrp.ProfileVisits([1], [1])])


# --- the two costs -----------------------------------------------------------


def test_the_forbidden_cost_counts_the_forbidden_visits_on_each_vehicle():
    model_input = line_input({"basic": {"forbidden_visits": ["a", "c"]}})
    name = "profile_forbidden_visit_cost"
    assert costs(model_input, name, {"v": ["a", "b", "c"]}) == [2, 0, 0]
    assert costs(model_input, name, {"w": ["a", "b", "c"]}) == [0, 0, 0]
    assert costs(model_input, name, {"v": ["b"]}) == [0, 0, 0]  # a and c unassigned: no vehicle's


def test_the_preferred_cost_counts_preferred_visits_on_a_vehicle_that_does_not_prefer_them():
    model_input = line_input({"skilled": {"preferred_visits": ["a"]}, "basic": {"preferred_visits": ["c"]}})
    name = "profile_preferred_visit_cost"
    # a is the skilled profile's, c the basic one's; b nobody's.
    assert costs(model_input, name, {"v": ["a", "b"], "w": ["c"]}) == [1, 1, 0]
    assert costs(model_input, name, {"v": ["b", "c"], "w": ["a"]}) == [0, 0, 0]
    assert costs(model_input, name, {"v": ["b"]}) == [0, 0, 0]


def test_a_preference_of_a_profile_no_vehicle_drives_on_counts_nowhere():
    model_input = line_input({})
    data = model_input.model_dump()
    data["profiles"]["idle"] = {"same_travel_as": "road", "preferred_visits": ["a"]}
    model_input = RoutingInput.model_validate(data)
    assert to_plan(model_input).profile_visits == []
    assert costs(model_input, "profile_preferred_visit_cost", {"v": ["a"]}) == [0, 0, 0]


def test_the_two_costs_score_with_their_default_weights_and_levels():
    model_input = line_input({"basic": {"forbidden_visits": ["c"]}, "skilled": {"preferred_visits": ["a"]}})
    plan = to_plan(model_input)
    plan.vehicles[0].visit_indices = [0, 2]  # v serves a (w's preference) and c (forbidden to v)
    plan.vehicles = plan.vehicles
    plan.rebuild_unassigned_pool()
    info = {entry.name: entry for entry in vrp.constraint_info()}
    assert (
        info["profile_forbidden_visit_cost"].default_level,
        info["profile_forbidden_visit_cost"].default_weight,
    ) == (
        "hard",
        1000.0,
    )
    assert (
        info["profile_preferred_visit_cost"].default_level,
        info["profile_preferred_visit_cost"].default_weight,
    ) == (
        "soft",
        600.0,
    )
    vrp.calculate_score(plan)
    assert plan.score.hard == -1000
    travel = sum(vrp.constraint_costs(plan)["travel_time_cost"]["costs"])
    assert plan.score.soft == -(travel + 600)
    assert score(vrp.constraint_costs(plan)) == (-1000, -(travel + 600) + plan.score.medium * 10**9)


# --- solves ------------------------------------------------------------------


@pytest.mark.parametrize("timelimit", [0, 0.3])
def test_a_forbidden_visit_goes_to_a_vehicle_that_may_serve_it(timelimit):
    # a is right by v, but v's profile forbids it: w drives over.
    model_input = line_input({"basic": {"forbidden_visits": ["a"]}})

    solution = solve(model_input, timelimit)

    assert solution.feasible and not solution.unassigned
    assert vehicle_of(solution)["a"] == "w"


@pytest.mark.parametrize("timelimit", [0, 0.3])
def test_a_visit_no_vehicle_may_serve_stays_unassigned(timelimit):
    model_input = line_input({"basic": {"forbidden_visits": ["a"]}, "skilled": {"forbidden_visits": ["a", "c"]}})

    solution = solve(model_input, timelimit)

    assert solution.feasible
    assert sorted(solution.unassigned) == ["a"]
    assert vehicle_of(solution)["c"] == "v"


def test_a_forbidden_visit_in_an_initial_route_is_moved_off():
    model_input = line_input(
        {"basic": {"forbidden_visits": ["a"]}},
        vehicles=[vehicle("v", "basic", route=["a", "b"]), vehicle("w", "skilled")],
    )

    solution = solve(model_input)

    assert solution.feasible and vehicle_of(solution)["a"] == "w"


def test_forbidden_visits_hold_whether_the_constraints_list_their_cost_or_not():
    model_input = line_input({"basic": {"forbidden_visits": ["a"]}})
    picks = [VehicleCapacityCost(), UnassignedVisitsCost(), TravelTimeCost()]

    solution = solve(model_input, constraints=picks)

    assert vehicle_of(solution)["a"] == "w"
    plan = to_plan(model_input)
    plan.vehicles[0].visit_indices = [0]
    plan.vehicles = plan.vehicles
    plan.rebuild_unassigned_pool()
    raw = vrp.constraint_costs(plan, picks)
    assert sorted(raw) == sorted(
        ["vehicle_capacity_cost", "unassigned_visits_cost", "travel_time_cost", "profile_forbidden_visit_cost"]
    )
    assert score(raw, picks).hard == -1000
    # Without forbidden visits the picks are scored as they are.
    assert "profile_forbidden_visit_cost" not in vrp.constraint_costs(to_plan(line_input({})), picks)


@pytest.mark.parametrize(
    "pick", [ProfileForbiddenVisitCost(level="soft"), ProfileForbiddenVisitCost(weight=0)], ids=["soft", "weight 0"]
)
def test_a_softer_pick_of_the_forbidden_cost_does_not_lift_the_rule(pick):
    model_input = line_input({"basic": {"forbidden_visits": ["a"]}})
    picks = [*without(get_constraints(), "profile_forbidden_visit_cost"), pick]

    assert vehicle_of(solve(model_input, constraints=picks))["a"] == "w"
    plan = to_plan(model_input)
    plan.vehicles[0].visit_indices = [0]
    plan.vehicles = plan.vehicles
    plan.rebuild_unassigned_pool()
    raw = vrp.constraint_costs(plan, picks)
    # The pick scores as picked, and the default instance beside it as the rule.
    assert raw["profile_forbidden_visit_cost"]["level"] == ("soft" if pick.level else "hard")
    assert raw["profile_forbidden_visit_cost#2"]["level"] == "hard"
    assert score(raw, picks).hard == -1000
    # A hard pick at a weight is the rule itself: nothing is added.
    hard = [*without(get_constraints(), "profile_forbidden_visit_cost"), ProfileForbiddenVisitCost(weight=5)]
    assert "profile_forbidden_visit_cost#2" not in vrp.constraint_costs(plan, hard)


def test_preferences_are_honored_when_they_cost_little_driving():
    # b is 10 s nearer v than w would drive, under the ten minutes a preference is worth.
    places = {"v": 0, "w": 30, "a": 15, "b": 12, "c": 40}
    model_input = line_input({"skilled": {"preferred_visits": ["b"]}}, places=places)
    assert vehicle_of(solve(line_input({}, places=places)))["b"] == "v"

    solution = solve(model_input)

    assert solution.feasible and vehicle_of(solution)["b"] == "w"


def test_preferences_give_way_to_a_long_drive():
    # w would drive far past the ten minutes a preference is worth to take a.
    model_input = line_input({"skilled": {"preferred_visits": ["a"]}})

    solution = solve(model_input)

    assert vehicle_of(solution)["a"] == "v"


def test_preferred_visits_need_their_cost_among_the_constraints():
    model_input = line_input({"skilled": {"preferred_visits": ["a"]}})
    picks = [VehicleCapacityCost(), UnassignedVisitsCost(), TravelTimeCost()]

    with pytest.raises(ValueError, match="leave out profile_preferred_visit_cost"):
        solve(model_input, constraints=picks)
    plan = to_plan(model_input)
    with pytest.raises(ValueError, match="profile_preferred_visit_cost"):
        vrp.solve(plan, time_limit_seconds=0, constraints=picks)
    # Weighed at nothing is still a choice the caller made.
    solve(model_input, constraints=[*picks, ProfilePreferredVisitCost(weight=0)])


def test_a_group_goes_to_a_vehicle_that_may_serve_all_of_it():
    # a and b are one job; v may not serve b, so the group rides w.
    model_input = line_input(
        {"basic": {"forbidden_visits": ["b"]}}, visits=[visit("a", "g"), visit("b", "g"), visit("c")]
    )

    for timelimit in (0, 0.3):
        solution = solve(model_input, timelimit)
        assert solution.feasible and not solution.unassigned
        assert vehicle_of(solution)["a"] == vehicle_of(solution)["b"] == "w"


def test_a_solve_with_lists_scores_on_the_default_constraints():
    model_input = line_input({"basic": {"forbidden_visits": ["c"]}})
    plan = to_plan(model_input)
    vrp.solve(plan, time_limit_seconds=0.2, random_seed=1)
    assert plan.score.hard == 0
    assert all(vrp_visit.vehicle_index != 0 for vrp_visit in plan.visits if vrp_visit.id == "c")
