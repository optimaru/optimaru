"""Visit groups: visits the input has merged into one job ride one vehicle together, in a partial order."""

import random

import pytest

import optimaru.vehicle_routing as vrp
from optimaru.vehicle_routing.constraints import VisitGroupCost, VisitOrderCost, get_constraints
from optimaru.vehicle_routing.models import (
    RoutePlanRequest,
    RoutingInput,
    VehicleModel,
    VisitModel,
    solve_request,
    to_plan,
)


def visit(name: str, group: str | None = None, after=()) -> VisitModel:
    return VisitModel(
        id=name,
        location=(37.5, 127.0),
        max_end_time=10**7,
        service_duration=600,
        group=group,
        after=list(after),
    )


def vehicle(name: str, route=(), max_stops: int = 0) -> VehicleModel:
    return VehicleModel(id=name, capacity=10, depot=(37.5, 127.0), max_stops=max_stops, initial_route=list(route))


def line_input(vehicles, visits, places: dict[str, int]) -> RoutingInput:
    """Everything sits on a line; `places` gives each vehicle and visit its position."""
    order = [v.id for v in vehicles] + [v.id for v in visits]
    return RoutingInput(
        vehicles=vehicles,
        visits=visits,
        driving_seconds=[[abs(places[a] - places[b]) for b in order] for a in order],
    )


# `a` is by vehicle v; `b`, `c` and `d` are far away by vehicle w.
TWO_ENDS = {"v": 0, "w": 10_000, "a": 10, "b": 10_010, "c": 10_020, "d": 10_030}


def two_ends(visits, vehicles=None) -> RoutingInput:
    return line_input(vehicles or [vehicle("v"), vehicle("w")], visits, TWO_ENDS)


def solve(model_input: RoutingInput, timelimit: float = 0.3, constraints=None):
    return solve_request(RoutePlanRequest(model_input=model_input, timelimit=timelimit), constraints=constraints)


def vehicle_of(solution) -> dict[str, str]:
    return {stop.visit_id: name for name, route in solution.routes.items() for stop in route}


def order_of(solution, vehicle_id: str) -> list[str]:
    return [stop.visit_id for stop in solution.routes[vehicle_id]]


def costs(model_input: RoutingInput, name: str) -> float:
    return sum(vrp.constraint_costs(to_plan(model_input))[name]["costs"])


def test_visits_in_no_group_go_to_the_vehicle_nearest_each():
    solution = solve(two_ends([visit("a"), visit("b"), visit("c")]))

    assert solution.feasible
    assert vehicle_of(solution) == {"a": "v", "b": "w", "c": "w"}


def test_a_group_rides_one_vehicle_though_each_visit_alone_would_not():
    solution = solve(two_ends([visit("a", "g"), visit("b", "g"), visit("c")]))

    assert solution.feasible and not solution.unassigned
    riding = vehicle_of(solution)
    assert riding["a"] == riding["b"]


@pytest.mark.parametrize("timelimit", [0, 0.3])
def test_a_group_no_vehicle_takes_whole_stays_unassigned_together(timelimit):
    # Each vehicle takes one visit: the group of two fits neither.
    vehicles = [vehicle("v", max_stops=1), vehicle("w", max_stops=1)]

    solution = solve(two_ends([visit("a", "g"), visit("b", "g"), visit("c")], vehicles), timelimit)

    assert solution.feasible
    assert sorted(solution.unassigned) == ["a", "b"]
    assert "c" in vehicle_of(solution)


def test_after_puts_a_visit_behind_the_ones_it_names():
    # From w the cheapest order is b, c, d. `b` has to come after `d`.
    visits = [visit("a"), visit("b", "g", after=["d"]), visit("c", "g"), visit("d", "g")]

    solution = solve(two_ends(visits))

    assert solution.feasible and not solution.unassigned
    served = order_of(solution, "w")
    assert served.index("d") < served.index("b")
    assert sorted(served) == ["b", "c", "d"]


def test_without_an_order_the_same_group_is_served_the_cheapest_way():
    visits = [visit("a"), visit("b", "g"), visit("c", "g"), visit("d", "g")]

    solution = solve(two_ends(visits))

    assert order_of(solution, "w") == ["b", "c", "d"]


def test_an_order_is_partial_visits_it_does_not_relate_stay_free():
    # Only `d` before `b`: `c` goes wherever driving is least, between them here.
    visits = [visit("b", "g", after=["d"]), visit("c", "g"), visit("d", "g")]
    places = {"v": 0, "b": 30, "c": 20, "d": 10}

    solution = solve(line_input([vehicle("v")], visits, places))

    assert order_of(solution, "v") == ["d", "c", "b"]


@pytest.mark.parametrize("timelimit", [0, 0.3])
def test_construction_puts_waiting_visits_where_their_group_already_rides(timelimit):
    # `b` and `c` are next to w, but `a` of their group starts on v.
    vehicles = [vehicle("v", ["a"]), vehicle("w")]
    visits = [visit("a", "g"), visit("b", "g"), visit("c", "g")]

    solution = solve(two_ends(visits, vehicles), timelimit)

    assert solution.feasible
    assert len(set(vehicle_of(solution).values())) == 1
    if timelimit == 0:
        assert vehicle_of(solution) == {"a": "v", "b": "v", "c": "v"}


def test_a_group_whose_start_vehicle_has_no_room_for_the_rest_is_moved_whole():
    # `a` starts on v, which takes one visit. Construction offers `b` and `c` to v only, so they
    # wait; the search then takes the whole group to w.
    vehicles = [vehicle("v", ["a"], max_stops=1), vehicle("w", max_stops=3)]
    visits = [visit("a", "g"), visit("b", "g"), visit("c", "g")]
    model_input = two_ends(visits, vehicles)

    start = solve(model_input, timelimit=0)
    assert vehicle_of(start) == {"a": "v"} and sorted(start.unassigned) == ["b", "c"]

    solution = solve(model_input)
    assert solution.feasible and not solution.unassigned
    assert vehicle_of(solution) == {"a": "w", "b": "w", "c": "w"}


def test_groups_of_one_do_not_change_how_a_plan_is_scored():
    # Every visit names its own group, as input that carries a group on every order does. Nothing
    # is kept together, so the solve stays on the four core rules: the vehicles' `max_stops`,
    # which only the constraint assembly scores, does not hold a visit back.
    vehicles = [vehicle("v", max_stops=1), vehicle("w", max_stops=1)]
    plan = to_plan(two_ends([visit("a", "g1"), visit("b", "g2"), visit("c", "g3")], vehicles))
    config = vrp.SolverConfig()
    config.step_count_limit = 2000

    vrp.solve(plan, config=config)

    assert plan.unassigned_visit_ids() == []
    assert costs(two_ends([visit("a", "g1"), visit("b", "g2"), visit("c", "g3")], vehicles), "visit_group_cost") == 0

    # With a second visit in one of the groups the same call scores the default set: one visit
    # per vehicle, and the group of two fits neither.
    plan = to_plan(two_ends([visit("a", "g1"), visit("b", "g1"), visit("c", "g3")], vehicles))
    vrp.solve(plan, config=config)

    assert sorted(plan.unassigned_visit_ids()) == ["a", "b"]


def test_the_search_mends_a_group_that_starts_split_and_out_of_order():
    vehicles = [vehicle("v", ["b", "a"]), vehicle("w", ["c"])]
    visits = [visit("a", "g"), visit("b", "g", after=["a"]), visit("c", "g", after=["b"])]
    model_input = two_ends(visits, vehicles)
    assert costs(model_input, "visit_group_cost") == 1
    assert costs(model_input, "visit_order_cost") == 1

    solution = solve(model_input)

    assert solution.feasible and not solution.unassigned
    (served,) = [order_of(solution, name) for name in "vw" if solution.routes[name]]
    assert served == ["a", "b", "c"]


def test_the_group_cost_counts_the_visits_apart_from_most_of_their_group():
    grouped = [visit(name, "g") for name in "abc"]

    def on(*routes) -> float:
        vehicles = [vehicle(name, route) for name, route in zip("vwx", routes, strict=True)]
        return costs(line_input(vehicles, grouped, TWO_ENDS | {"x": 5_000}), "visit_group_cost")

    assert on(["a", "b", "c"], [], []) == 0  # together on one vehicle
    assert on([], [], []) == 0  # waiting together
    assert on(["a", "b"], ["c"], []) == 1  # one on another vehicle
    assert on(["a", "b"], [], []) == 1  # one left waiting
    assert on(["a"], [], []) == 1  # one riding while the others wait
    assert on(["a"], ["b"], ["c"]) == 2  # spread over three vehicles
    assert on(["a"], ["b"], []) == 2  # two vehicles and the pool


def test_the_order_cost_counts_the_pairs_served_the_wrong_way_round():
    # c after a and b; b after a.
    ordered = [visit("a", "g"), visit("b", "g", after=["a"]), visit("c", "g", after=["a", "b"])]

    def on(*routes) -> float:
        vehicles = [vehicle(name, route) for name, route in zip("vw", routes, strict=True)]
        return costs(two_ends(ordered, vehicles), "visit_order_cost")

    assert on(["a", "b", "c"], []) == 0
    assert on(["a", "c", "b"], []) == 1  # c before b
    assert on(["c", "b", "a"], []) == 3  # every pair reversed
    assert on(["b", "a"], ["c"]) == 1  # c rides elsewhere: only (a, b) is on this route
    assert on([], []) == 0  # nothing routed, nothing out of order


def test_a_group_of_one_costs_nothing_by_itself():
    vehicles = [vehicle("v", ["a"]), vehicle("w", ["b"])]

    assert costs(two_ends([visit("a", "g"), visit("b", "h"), visit("c")], vehicles), "visit_group_cost") == 0


def test_the_group_constraint_can_be_made_soft_or_left_out():
    # Each vehicle takes one visit, so the group cannot ride together. Kept hard it waits whole;
    # soft at a weight below what an unassigned visit costs, it is split over the two vehicles.
    vehicles = [vehicle("v", max_stops=1), vehicle("w", max_stops=1)]
    model_input = two_ends([visit("a", "g"), visit("b", "g")], vehicles)
    others = [spec for spec in get_constraints() if spec.name != "visit_group_cost"]

    soft = solve(model_input, constraints=[*others, VisitGroupCost(weight=1, level="soft")])
    without = solve(model_input, constraints=others)

    for solution in (soft, without):
        assert solution.feasible and not solution.unassigned
        assert vehicle_of(solution) == {"a": "v", "b": "w"}


def test_the_order_constraint_can_be_made_soft():
    # `b` after `d` costs 40 more seconds of driving on w; at 1 soft it is not worth keeping.
    visits = [visit("b", "g", after=["d"]), visit("c", "g"), visit("d", "g")]
    places = {"w": 10_000, "b": 10_010, "c": 10_020, "d": 10_030}
    others = [spec for spec in get_constraints() if spec.name != "visit_order_cost"]

    solution = solve(
        line_input([vehicle("w")], visits, places), constraints=[*others, VisitOrderCost(weight=1, level="soft")]
    )

    assert order_of(solution, "w") == ["b", "c", "d"]


@pytest.mark.parametrize(
    ("visits", "message"),
    [
        ([visit("a", "g", after=["z"])], "visit a: `after` names an unknown visit 'z'"),
        ([visit("a", "g", after=["a"])], "visit a comes after itself"),
        ([visit("a", "g", after=["b"]), visit("b", "h")], "visit a comes after visit b, which is not in its group"),
        ([visit("a", "g", after=["b"]), visit("b")], "visit a comes after visit b, which is not in its group"),
        ([visit("a", after=["b"]), visit("b")], "visit a comes after visit b, which is not in its group"),
        ([visit("a", "g", after=["b", "b"]), visit("b", "g")], "visit a: `after` names a visit twice"),
        (
            [visit("a", "g", after=["c"]), visit("b", "g", after=["a"]), visit("c", "g", after=["b"])],
            r"the order of the visits has a cycle through \['a', 'b', 'c'\]",
        ),
    ],
)
def test_an_order_that_cannot_be_served_is_refused(visits, message):
    places = {"v": 0, "a": 10, "b": 20, "c": 30}

    with pytest.raises(ValueError, match=message):
        line_input([vehicle("v")], visits, places)


def test_the_engine_refuses_the_same_on_a_plan():
    plan = to_plan(two_ends([visit("a", "g"), visit("b", "g"), visit("c")]))
    visits = plan.visits
    visits[0].after_visit_indices = [1]
    visits[1].after_visit_indices = [0]
    cycle = vrp.VehicleRoutePlan(plan.name, plan.locations, plan.vehicles, visits)
    with pytest.raises(ValueError, match="the order of the visits has a cycle through visit a"):
        vrp.solve(cycle, time_limit_seconds=0.1)

    visits = plan.visits
    visits[0].after_visit_indices = [2]
    stranger = vrp.VehicleRoutePlan(plan.name, plan.locations, plan.vehicles, visits)
    with pytest.raises(ValueError, match="visit a comes after visit c, which is not in its group"):
        vrp.solve(stranger, time_limit_seconds=0.1)

    visits = plan.visits
    visits[0].after_visit_indices = [1, 1]
    twice = vrp.VehicleRoutePlan(plan.name, plan.locations, plan.vehicles, visits)
    with pytest.raises(ValueError, match="visit a names visit b twice among the visits it comes after"):
        vrp.solve(twice, time_limit_seconds=0.1)


def test_an_empty_group_name_is_no_group():
    model_input = two_ends([visit("a", ""), visit("b", ""), visit("c")])

    assert [v.group_index for v in to_plan(model_input).visits] == [-1, -1, -1]
    assert vehicle_of(solve(model_input)) == {"a": "v", "b": "w", "c": "w"}


def test_to_plan_numbers_the_groups_in_input_order_and_resolves_after():
    visits = [visit("a", "late", after=["d"]), visit("b"), visit("c", "early"), visit("d", "late")]

    plan = to_plan(two_ends(visits))

    assert [v.group_index for v in plan.visits] == [0, -1, 1, 0]
    assert [v.after_visit_indices for v in plan.visits] == [[3], [], [], []]


@pytest.mark.parametrize("entry", ["solve", "Solver.solve"])
def test_every_entry_point_scores_groups_on_a_plan_without_being_asked(entry):
    # No vehicle takes both visits of the group (one visit each), so it has to wait whole.
    # Scored on the four core rules alone, routing the two apart would look feasible and better.
    vehicles = [vehicle("v", max_stops=1), vehicle("w", max_stops=1)]
    plan = to_plan(two_ends([visit("a", "g"), visit("b", "g")], vehicles))
    config = vrp.SolverConfig()
    config.step_count_limit = 3000

    if entry == "solve":
        vrp.solve(plan, config=config)
    else:
        vrp.Solver(config).solve(plan)

    assert plan.score.is_feasible()
    assert sorted(plan.unassigned_visit_ids()) == ["a", "b"]
    assert plan.score.medium == -20  # two visits of 10 service minutes each


def as_tuple(score) -> tuple[int, int, int]:
    return (score.hard, score.medium, score.soft)


def test_calculate_score_reads_a_grouped_plan_as_the_solve_scored_it():
    # No vehicle takes both visits of the group, so the solve leaves them waiting together.
    vehicles = [vehicle("v", max_stops=1), vehicle("w", max_stops=1)]
    plan = to_plan(two_ends([visit("a", "g"), visit("b", "g"), visit("c")], vehicles))
    config = vrp.SolverConfig()
    config.step_count_limit = 3000
    vrp.solve(plan, config=config)
    solved = as_tuple(plan.score)

    assert as_tuple(vrp.calculate_score(plan)) == solved
    assert as_tuple(plan.score) == solved


@pytest.mark.parametrize(
    ("routes", "hard"),
    [
        ((["a", "b"], []), 0),  # together, in order
        ((["a"], ["b"]), -1000),  # split: one visit apart from its group
        ((["b", "a"], []), -1000),  # together, the wrong way round
        ((["b"], []), -1000),  # one riding while the other waits
    ],
)
def test_calculate_score_prices_groups_and_order_on_a_plan_built_by_hand(routes, hard):
    vehicles = [vehicle(name, route) for name, route in zip("vw", routes, strict=True)]
    plan = to_plan(two_ends([visit("a", "g"), visit("b", "g", after=["a"])], vehicles))

    score = vrp.calculate_score(plan)

    assert score.hard == hard
    assert as_tuple(plan.score) == as_tuple(score)
    assert score.hard == -1000 * sum(
        sum(vrp.constraint_costs(plan)[name]["costs"]) for name in ("visit_group_cost", "visit_order_cost")
    )


def test_calculate_score_refuses_an_order_the_solve_refuses():
    plan = to_plan(two_ends([visit("a", "g"), visit("b", "g"), visit("c")]))
    visits = plan.visits
    visits[0].after_visit_indices = [2]
    stranger = vrp.VehicleRoutePlan(plan.name, plan.locations, plan.vehicles, visits)

    with pytest.raises(ValueError, match="visit a comes after visit c, which is not in its group"):
        vrp.calculate_score(stranger)


def test_a_constraint_list_without_the_group_rule_is_used_as_it_is():
    # The group starts on two vehicles and neither takes both visits. With the caller's own
    # list, which leaves `visit_group_cost` out, nothing in the score is wrong with that.
    vehicles = [vehicle("v", ["a"], max_stops=1), vehicle("w", ["b"], max_stops=1)]
    plan = to_plan(two_ends([visit("a", "g"), visit("b", "g")], vehicles))
    config = vrp.SolverConfig()
    config.step_count_limit = 500
    others = [spec for spec in get_constraints() if spec.name != "visit_group_cost"]

    vrp.solve(plan, config=config, constraints=others)

    assert plan.score.is_feasible()
    assert plan.routes() == {"v": ["a"], "w": ["b"]}


def test_the_group_operator_can_be_turned_off_and_its_knob_is_checked():
    config = vrp.SolverConfig()
    assert config.group_ruin_recreate is True
    assert config.group_ruin_max_visits == 30
    config.group_ruin_recreate = False
    config.group_ruin_max_visits = 0
    with pytest.raises(ValueError, match="group_ruin_max_visits must not be negative"):
        config.group_ruin_max_visits = -1

    solution = solve_request(
        RoutePlanRequest(
            model_input=two_ends([visit("a", "g"), visit("b", "g"), visit("c")]),
            timelimit=0.3,
            solver_parameters={"group_ruin_recreate": False},
        )
    )
    riding = vehicle_of(solution)
    assert solution.feasible and riding["a"] == riding["b"]


def grouped_input(visits: int, vehicles: int, size: int, seed: int) -> RoutingInput:
    """Random visits with morning or afternoon windows, the first two thirds in groups of `size`,
    each group a chain in the order its windows open."""
    rng = random.Random(seed)
    stops = []
    for index in range(visits):
        morning = rng.random() < 0.5
        stops.append(
            VisitModel(
                id=f"s{index}",
                location=(43.75 + 0.06 * rng.random(), 11.18 + 0.11 * rng.random()),
                min_start_time=(8 if morning else 13) * 3600,
                max_end_time=(12 if morning else 18) * 3600,
                service_duration=600,
            )
        )
    for first in range(0, 2 * visits // 3, size):
        members = sorted(stops[first : first + size], key=lambda stop: stop.min_start_time)
        for position, stop in enumerate(members):
            stop.group = f"g{first // size}"
            stop.after = [members[position - 1].id] if position else []
    fleet = [
        VehicleModel(id=f"v{index}", capacity=visits, depot=(43.78, 11.23), departure_time=7 * 3600 + 1800)
        for index in range(vehicles)
    ]
    return RoutingInput(vehicles=fleet, visits=stops)


def groups_are_whole_and_in_order(model_input: RoutingInput, solution) -> bool:
    place = {
        stop.visit_id: (name, position)
        for name, route in solution.routes.items()
        for position, stop in enumerate(route)
    }
    riding: dict[str, set] = {}
    for stop in model_input.visits:
        if stop.group:
            riding.setdefault(stop.group, set()).add(place.get(stop.id, (None, None))[0])
        for earlier in stop.after:
            if stop.id in place and earlier in place and place[earlier][1] > place[stop.id][1]:
                return False
    return all(len(where) == 1 for where in riding.values())


@pytest.mark.parametrize("group_operator", [True, False])
def test_groups_stay_whole_and_in_order_in_a_larger_solve(group_operator):
    model_input = grouped_input(36, 4, 3, seed=3)
    request = RoutePlanRequest(
        model_input=model_input,
        timelimit=60,  # the step limit ends the solve
        random_seed=3,
        solver_parameters={"step_count_limit": 6000, "full_assert": True, "group_ruin_recreate": group_operator},
    )

    solution = solve_request(request)

    assert solution.feasible
    assert not solution.unassigned
    assert groups_are_whole_and_in_order(model_input, solution)
