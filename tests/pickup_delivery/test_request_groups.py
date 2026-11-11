"""Request groups: requests the input has merged into one job ride one vehicle together."""

import pytest

import optimaru.pickup_delivery as pdp
from optimaru.pickup_delivery.constraints import RequestGroupCost, get_constraints
from optimaru.pickup_delivery.models import (
    DeliveryModel,
    PdpInput,
    PdpRequest,
    StopModel,
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


def vehicle(name: str, route=(), pinned: int = 0, max_stops: int = 0) -> VehicleModel:
    return VehicleModel(
        id=name,
        capacity=5,
        home=(37.5, 127.0),
        max_stops=max_stops,
        initial_route=list(route),
        pinned_stops=pinned,
    )


def line_input(vehicles, deliveries, places: dict[str, int]) -> PdpInput:
    """Everything sits on a line; `places` gives each vehicle and stop its position."""
    order = [v.id for v in vehicles]
    for delivery in deliveries:
        order += [delivery.pickup.id, delivery.dropoff.id]
    return PdpInput(
        vehicles=vehicles,
        deliveries=deliveries,
        driving_seconds=[[abs(places[a] - places[b]) for b in order] for a in order],
    )


def vehicle_of(solution) -> dict[str, str]:
    return {stop.delivery_id: name for name, route in solution.routes.items() for stop in route}


# `a` is by vehicle v, `b` and `c` are far away by vehicle w.
TWO_ENDS = {"v": 0, "w": 10_000, "ap": 10, "ad": 20, "bp": 10_010, "bd": 10_020, "cp": 10_030, "cd": 10_040}


def two_ends(deliveries, vehicles=None) -> PdpInput:
    return line_input(vehicles or [vehicle("v"), vehicle("w")], deliveries, TWO_ENDS)


def apart(model_input: PdpInput) -> float:
    return sum(pdp.constraint_costs(to_plan(model_input))["request_group_cost"]["costs"])


def test_requests_in_no_group_go_to_the_vehicle_nearest_each():
    solution = solve_request(
        PdpRequest(model_input=two_ends([request("a"), request("b"), request("c")]), timelimit=0.3)
    )

    assert solution.feasible
    assert vehicle_of(solution) == {"a": "v", "b": "w", "c": "w"}


def test_a_group_rides_one_vehicle_though_each_request_alone_would_not():
    deliveries = [request("a", group="g"), request("b", group="g"), request("c")]

    solution = solve_request(PdpRequest(model_input=two_ends(deliveries), timelimit=0.3))

    assert solution.feasible and not solution.unassigned
    riding = vehicle_of(solution)
    assert riding["a"] == riding["b"]


@pytest.mark.parametrize("timelimit", [0, 0.3])
def test_a_group_no_vehicle_takes_whole_stays_unassigned_together(timelimit):
    # Each vehicle takes one request (two stops): the group of two fits neither.
    vehicles = [vehicle("v", max_stops=2), vehicle("w", max_stops=2)]
    deliveries = [request("a", group="g"), request("b", group="g"), request("c")]

    solution = solve_request(PdpRequest(model_input=two_ends(deliveries, vehicles), timelimit=timelimit))

    assert solution.feasible
    assert sorted(solution.unassigned) == ["a", "b"]
    assert "c" in vehicle_of(solution)


@pytest.mark.parametrize("timelimit", [0, 0.3])
def test_a_required_vehicle_on_one_request_takes_its_group_there(timelimit):
    # `a` is next to v, yet `b` must ride w and the two travel together.
    deliveries = [request("a", group="g"), request("b", group="g", required_vehicle="w"), request("c")]

    solution = solve_request(PdpRequest(model_input=two_ends(deliveries), timelimit=timelimit))

    assert solution.feasible
    riding = vehicle_of(solution)
    assert riding["a"] == riding["b"] == "w"


@pytest.mark.parametrize("timelimit", [0, 0.3])
def test_a_group_joins_its_pinned_request(timelimit):
    # `b` is already on v and its pickup cannot move, so `a` and `c` join it there.
    vehicles = [vehicle("v", ["bp", "bd"], pinned=1), vehicle("w")]
    deliveries = [request("a", group="g"), request("b", group="g"), request("c", group="g")]

    solution = solve_request(PdpRequest(model_input=two_ends(deliveries, vehicles), timelimit=timelimit))

    assert solution.feasible
    assert vehicle_of(solution) == {"a": "v", "b": "v", "c": "v"}
    assert solution.routes["v"][0].stop_id == "bp"


def test_a_request_that_starts_off_its_groups_required_vehicle_is_brought_over():
    # `b` starts on v, unpinned, while `a` of its group must ride w.
    vehicles = [vehicle("v", ["bp", "bd"]), vehicle("w")]
    deliveries = [request("a", group="g", required_vehicle="w"), request("b", group="g"), request("c")]

    solution = solve_request(PdpRequest(model_input=two_ends(deliveries, vehicles), timelimit=0.3))

    assert solution.feasible and not solution.unassigned
    riding = vehicle_of(solution)
    assert riding["a"] == riding["b"] == "w"


def test_construction_puts_waiting_requests_where_their_group_already_rides():
    # `b` and `c` are next to w, but `a` of their group starts on v.
    vehicles = [vehicle("v", ["ap", "ad"]), vehicle("w")]
    deliveries = [request("a", group="g"), request("b", group="g"), request("c", group="g")]

    solution = solve_request(PdpRequest(model_input=two_ends(deliveries, vehicles), timelimit=0))

    assert solution.feasible
    assert vehicle_of(solution) == {"a": "v", "b": "v", "c": "v"}


def test_the_search_mends_a_group_that_starts_on_two_vehicles():
    vehicles = [vehicle("v", ["ap", "ad"]), vehicle("w", ["bp", "bd", "cp", "cd"])]
    deliveries = [request("a", group="g"), request("b", group="g"), request("c")]
    model_input = two_ends(deliveries, vehicles)
    assert apart(model_input) == 1

    solution = solve_request(PdpRequest(model_input=model_input, timelimit=0.3))

    assert solution.feasible and not solution.unassigned
    riding = vehicle_of(solution)
    assert riding["a"] == riding["b"]


def test_the_cost_counts_the_requests_apart_from_most_of_their_group():
    grouped = [request(name, group="g") for name in "abc"]

    def on(*routes) -> float:
        vehicles = [vehicle(name, route) for name, route in zip("vwx", routes)]
        places = TWO_ENDS | {"x": 5_000}
        return apart(line_input(vehicles, grouped, places))

    assert on(["ap", "ad", "bp", "bd", "cp", "cd"], [], []) == 0  # together on one vehicle
    assert on([], [], []) == 0  # waiting together
    assert on(["ap", "ad", "bp", "bd"], ["cp", "cd"], []) == 1  # one on another vehicle
    assert on(["ap", "ad", "bp", "bd"], [], []) == 1  # one left waiting
    assert on(["ap", "ad"], [], []) == 1  # one riding while the others wait
    assert on(["ap", "ad"], ["bp", "bd"], ["cp", "cd"]) == 2  # spread over three vehicles
    assert on(["ap", "ad"], ["bp", "bd"], []) == 2  # two vehicles and the pool


def test_a_group_of_one_and_two_groups_cost_nothing_by_themselves():
    deliveries = [request("a", group="g"), request("b", group="h"), request("c")]
    vehicles = [vehicle("v", ["ap", "ad"]), vehicle("w", ["bp", "bd"])]

    assert apart(two_ends(deliveries, vehicles)) == 0


def test_the_group_constraint_can_be_made_soft_or_left_out():
    # Each vehicle takes one request, so the group cannot ride together. Kept hard it waits whole;
    # soft at a weight below what an unassigned request costs, it is split over the two vehicles.
    vehicles = [vehicle("v", max_stops=2), vehicle("w", max_stops=2)]
    deliveries = [request("a", group="g"), request("b", group="g")]
    model_input = two_ends(deliveries, vehicles)
    others = [spec for spec in get_constraints() if spec.name != "request_group_cost"]

    soft = solve_request(
        PdpRequest(model_input=model_input, timelimit=0.3),
        constraints=[*others, RequestGroupCost(weight=1, level="soft")],
    )
    without = solve_request(PdpRequest(model_input=model_input, timelimit=0.3), constraints=others)

    for solution in (soft, without):
        assert solution.feasible and not solution.unassigned
        assert vehicle_of(solution) == {"a": "v", "b": "w"}


def test_requests_of_one_group_cannot_require_different_vehicles():
    deliveries = [request("a", group="g", required_vehicle="v"), request("b", group="g", required_vehicle="w")]

    with pytest.raises(ValueError, match=r"group 'g': requests a and b are bound to different vehicles \(v and w\)"):
        two_ends([*deliveries, request("c")])


def test_a_group_cannot_be_pinned_on_one_vehicle_and_required_on_another():
    vehicles = [vehicle("v", ["ap", "ad"], pinned=1), vehicle("w")]
    deliveries = [request("a", group="g"), request("b", group="g", required_vehicle="w"), request("c")]

    with pytest.raises(ValueError, match=r"group 'g': requests a and b are bound to different vehicles \(v and w\)"):
        two_ends(deliveries, vehicles)


def test_a_group_cannot_be_pinned_on_two_vehicles():
    vehicles = [vehicle("v", ["ap", "ad"], pinned=2), vehicle("w", ["bp", "bd"], pinned=1)]
    deliveries = [request("a", group="g"), request("b", group="g"), request("c")]

    with pytest.raises(ValueError, match=r"group 'g': requests a and b are bound to different vehicles \(v and w\)"):
        two_ends(deliveries, vehicles)


def test_a_group_pinned_and_required_on_the_same_vehicle_is_fine():
    vehicles = [vehicle("v", ["ap", "ad"], pinned=1), vehicle("w")]
    deliveries = [request("a", group="g"), request("b", group="g", required_vehicle="v"), request("c")]

    solution = solve_request(PdpRequest(model_input=two_ends(deliveries, vehicles), timelimit=0.3))

    assert solution.feasible
    riding = vehicle_of(solution)
    assert riding["a"] == riding["b"] == "v"


def test_an_unpinned_start_on_two_vehicles_is_not_a_clash():
    # Nothing holds `a` or `b` where they start, so the group can still come together.
    vehicles = [vehicle("v", ["ap", "ad"]), vehicle("w", ["bp", "bd"])]
    deliveries = [request("a", group="g"), request("b", group="g"), request("c", group="g")]

    solution = solve_request(PdpRequest(model_input=two_ends(deliveries, vehicles), timelimit=0.3))

    assert solution.feasible and not solution.unassigned
    assert len(set(vehicle_of(solution).values())) == 1


def test_construction_adds_waiting_requests_to_the_vehicle_that_holds_most_of_their_group():
    # `a` starts on v, `b` and `c` on w; `d` waits and is as near to v as to w.
    places = {"v": 0, "w": 100, "ap": 10, "ad": 20, "bp": 110, "bd": 120, "cp": 130, "cd": 140, "dp": 50, "dd": 60}
    vehicles = [vehicle("v", ["ap", "ad"]), vehicle("w", ["bp", "bd", "cp", "cd"])]
    deliveries = [request(name, group="g") for name in "abcd"]

    solution = solve_request(PdpRequest(model_input=line_input(vehicles, deliveries, places), timelimit=0))

    assert vehicle_of(solution) == {"a": "v", "b": "w", "c": "w", "d": "w"}


def test_the_engine_refuses_the_same_clash_on_a_plan():
    plan = to_plan(two_ends([request("a", group="g"), request("b", group="g"), request("c")]))
    deliveries = plan.deliveries
    deliveries[0].required_vehicle_index = 0
    deliveries[1].required_vehicle_index = 1
    clash = pdp.PdpRoutePlan(plan.name, plan.locations, plan.vehicles, plan.nodes, deliveries)

    with pytest.raises(ValueError, match="requests a and b share a group but are bound to different vehicles"):
        pdp.solve(clash, time_limit_seconds=0.1, constraints=get_constraints())


@pytest.mark.parametrize("pair_construction", [True, False])
def test_a_refused_plan_is_left_as_it_was_and_can_still_be_scored(pair_construction):
    plan = to_plan(two_ends([request("a", group="g"), request("b", group="g"), request("c")]))
    deliveries = plan.deliveries
    deliveries[0].required_vehicle_index = 0
    deliveries[1].required_vehicle_index = 1
    clash = pdp.PdpRoutePlan(plan.name, plan.locations, plan.vehicles, plan.nodes, deliveries)
    clash.rebuild_unassigned_pool()
    waiting = list(clash.unassigned_node_indices)
    config = pdp.SolverConfig()
    config.step_count_limit = 100
    config.pair_construction_heuristic = pair_construction

    # No constraint list: the engine's own switch for a grouped plan, which with the pair
    # construction off empties the pool before it solves.
    with pytest.raises(ValueError, match="requests a and b share a group but are bound to different vehicles"):
        pdp.Solver(config).solve(clash)

    assert len(waiting) == 6
    assert list(clash.unassigned_node_indices) == waiting

    # calculate_score does not validate: the plan is scored as it stands, all of it waiting
    # (two requests off the vehicle they require, three requests of 2 service minutes each).
    score = pdp.calculate_score(clash)
    assert (score.hard, score.medium) == (-2000, -6)


def test_the_engine_refuses_a_group_pinned_on_two_vehicles():
    plan = to_plan(two_ends([request("a", group="g"), request("b", group="g"), request("c")]))
    vehicles = plan.vehicles
    by_id = {node.id: index for index, node in enumerate(plan.nodes)}
    vehicles[0].node_indices = [by_id["ap"], by_id["ad"]]
    vehicles[0].pinned_node_indices = [by_id["ap"]]
    vehicles[1].node_indices = [by_id["bp"], by_id["bd"]]
    vehicles[1].pinned_node_indices = [by_id["bp"]]
    clash = pdp.PdpRoutePlan(plan.name, plan.locations, vehicles, plan.nodes, plan.deliveries)
    clash.rebuild_unassigned_pool()

    with pytest.raises(ValueError, match="requests a and b share a group but are bound to different vehicles"):
        pdp.solve(clash, time_limit_seconds=0.1, constraints=get_constraints())


def test_an_empty_group_name_is_no_group():
    # Like an empty required_vehicle: input built from a table often carries "" for "none".
    deliveries = [request("a", group=""), request("b", group=""), request("c")]
    model_input = two_ends(deliveries)

    assert [delivery.group_index for delivery in to_plan(model_input).deliveries] == [-1, -1, -1]
    solution = solve_request(PdpRequest(model_input=model_input, timelimit=0.3))
    assert vehicle_of(solution) == {"a": "v", "b": "w", "c": "w"}


@pytest.mark.parametrize("entry", ["solve", "Solver.solve"])
def test_every_entry_point_scores_groups_on_a_plan_without_being_asked(entry):
    # No vehicle takes both requests of the group (one request each), so it has to wait whole.
    # Scored on the seven core constraints alone, routing the two apart would look feasible and better.
    vehicles = [vehicle("v", max_stops=2), vehicle("w", max_stops=2)]
    plan = to_plan(two_ends([request("a", group="g"), request("b", group="g")], vehicles))
    config = pdp.SolverConfig()
    config.step_count_limit = 3000

    if entry == "solve":
        pdp.solve(plan, config=config)
    else:
        pdp.Solver(config).solve(plan)

    assert plan.score.is_feasible()
    assert sorted(plan.unassigned_delivery_ids()) == ["a", "b"]
    assert plan.score.medium == -4  # two requests of 2 service minutes each


def as_tuple(score) -> tuple[int, int, int]:
    return (score.hard, score.medium, score.soft)


def test_calculate_score_reads_a_grouped_plan_as_the_solve_scored_it():
    vehicles = [vehicle("v", max_stops=2), vehicle("w", max_stops=2)]
    plan = to_plan(two_ends([request("a", group="g"), request("b", group="g"), request("c")], vehicles))
    config = pdp.SolverConfig()
    config.step_count_limit = 3000
    pdp.solve(plan, config=config)
    solved = as_tuple(plan.score)

    assert as_tuple(pdp.calculate_score(plan)) == solved
    assert as_tuple(plan.score) == solved


@pytest.mark.parametrize(
    ("routes", "hard"),
    [
        ((["ap", "ad", "bp", "bd"], []), 0),  # together
        ((["ap", "ad"], ["bp", "bd"]), -1000),  # split: one request apart from its group
        ((["bp", "bd"], []), -1000),  # one riding while the other waits
    ],
)
def test_calculate_score_prices_a_split_group_on_a_plan_built_by_hand(routes, hard):
    vehicles = [vehicle(name, route) for name, route in zip("vw", routes, strict=True)]
    plan = to_plan(two_ends([request("a", group="g"), request("b", group="g")], vehicles))

    score = pdp.calculate_score(plan)

    assert score.hard == hard
    assert as_tuple(plan.score) == as_tuple(score)
    assert score.hard == -1000 * sum(pdp.constraint_costs(plan)["request_group_cost"]["costs"])


def test_groups_of_one_do_not_change_how_a_plan_is_scored():
    # Every request names its own group, as input that carries a group on every order does.
    # Nothing is kept together, so the solve stays on the seven core constraints: the vehicles'
    # `max_stops`, which only the constraint assembly scores, does not hold a request back.
    vehicles = [vehicle("v", max_stops=2), vehicle("w", max_stops=2)]
    alone = [request("a", group="g1"), request("b", group="g2"), request("c", group="g3")]
    plan = to_plan(two_ends(alone, vehicles))
    config = pdp.SolverConfig()
    config.step_count_limit = 2000

    pdp.solve(plan, config=config)

    assert plan.unassigned_delivery_ids() == []
    assert plan.score.hard == 0
    assert as_tuple(pdp.calculate_score(plan)) == as_tuple(plan.score)  # the core constraints there too

    # With a second request in one of the groups the same call scores the default set: one
    # request per vehicle, and the group of two fits neither.
    plan = to_plan(two_ends([request("a", group="g1"), request("b", group="g1"), request("c", group="g3")], vehicles))
    pdp.solve(plan, config=config)

    assert sorted(plan.unassigned_delivery_ids()) == ["a", "b"]


def test_a_constraint_list_without_the_group_rule_is_used_as_it_is():
    # The group starts on two vehicles and neither takes both requests. With the caller's own
    # list, which leaves `request_group_cost` out, nothing in the score is wrong with that.
    vehicles = [vehicle("v", ["ap", "ad"], max_stops=2), vehicle("w", ["bp", "bd"], max_stops=2)]
    model_input = two_ends([request("a", group="g"), request("b", group="g")], vehicles)
    config = pdp.SolverConfig()
    config.step_count_limit = 500
    others = [spec for spec in get_constraints() if spec.name != "request_group_cost"]

    plan = to_plan(model_input)
    pdp.solve(plan, config=config, constraints=others)

    assert plan.score.is_feasible()
    assert plan.unassigned_delivery_ids() == []
    assert plan.routes() == {"v": ["ap", "ad"], "w": ["bp", "bd"]}

    # The default set does score it: the split group is a hard violation until it waits whole.
    plan = to_plan(model_input)
    pdp.solve(plan, config=config)

    assert plan.score.is_feasible()
    assert sorted(plan.unassigned_delivery_ids()) == ["a", "b"]


def test_to_plan_numbers_the_groups_in_input_order():
    deliveries = [request("a", group="late"), request("b"), request("c", group="early"), request("d", group="late")]
    places = TWO_ENDS | {"dp": 30, "dd": 40}

    plan = to_plan(line_input([vehicle("v")], deliveries, places))

    assert [delivery.group_index for delivery in plan.deliveries] == [0, -1, 1, 0]


def grouped_instance(requests: int, vehicles: int, size: int, seed: int) -> pdp.PdpRoutePlan:
    base = pdp.build_random_instance(requests, vehicles, seed)
    deliveries = base.deliveries
    for index, delivery in enumerate(deliveries):
        delivery.group_index = index // size
    return pdp.PdpRoutePlan(base.name, base.locations, base.vehicles, base.nodes, deliveries)


def groups_are_whole(plan: pdp.PdpRoutePlan) -> bool:
    riding = {}
    for index, route in enumerate(v.node_indices for v in plan.vehicles):
        for node in route:
            riding[node] = index
    places: dict[int, set] = {}
    for delivery in plan.deliveries:
        places.setdefault(delivery.group_index, set()).add(riding.get(delivery.pickup_node_index))
    return all(len(where) == 1 for where in places.values())


@pytest.mark.parametrize("path", ["groups", "a constraint list"])
def test_parallel_starts_return_the_best_single_start_on_the_constraint_path(path):
    # Start i searches with random_seed + i on constraints of its own and the best score wins,
    # so three step-limited starts return what the best of three single-start solves does.
    def score(seed: int, starts: int) -> tuple[int, int, int]:
        config = pdp.SolverConfig()
        config.spent_limit_seconds = 60.0  # the step limit ends the solve
        config.step_count_limit = 2000
        config.random_seed = seed
        config.parallel_solver_count = starts
        plan = grouped_instance(18, 3, 3, seed=3)
        pdp.solve(plan, config=config, constraints=get_constraints() if path == "a constraint list" else None)
        return as_tuple(plan.score)

    for seed in (0, 10):
        singles = [score(seed + start, 1) for start in range(3)]
        assert score(seed, 1) == singles[0], "a step-limited start is not reproducible"
        assert score(seed, 3) == max(singles)


def test_a_grouped_request_with_num_workers_solves():
    # A smoke test of the path a request takes (solve_request, Solver.solve, the engine's own
    # switch, three starts): a problem this small has one answer whatever the starts.
    deliveries = [request(name, group="g" if name in "ab" else None) for name in "abc"]

    def score(seed: int, workers: int) -> str:
        return solve_request(
            PdpRequest(
                model_input=two_ends(deliveries),
                timelimit=60,
                random_seed=seed,
                num_workers=workers,
                solver_parameters={"step_count_limit": 500},
            )
        ).score

    assert score(0, 3) == score(0, 1)
    assert "0hard" in score(0, 3)


@pytest.mark.parametrize("request_moves", [True, False])
def test_groups_stay_whole_in_a_larger_solve(request_moves):
    plan = grouped_instance(24, 4, 3, seed=3)
    config = pdp.SolverConfig()
    config.step_count_limit = 6000
    config.full_assert = True
    config.request_ruin_recreate = request_moves
    config.bundle_moves = request_moves

    pdp.solve(plan, random_seed=3, config=config, constraints=get_constraints())

    assert plan.score.is_feasible()
    assert not plan.unassigned_node_indices
    assert groups_are_whole(plan)
