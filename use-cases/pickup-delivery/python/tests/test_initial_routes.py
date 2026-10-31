"""Solving from a given route, with its first stops pinned."""

import random

import pdp_routing as pdp
import pytest
from pdp_routing.constraints import MinimizeTravelTime, get_constraints
from pdp_routing.models import DeliveryModel, PdpInput, PdpRequest, StopModel, VehicleModel, solve_request


def stop(name: str, service: int = 0) -> StopModel:
    return StopModel(id=name, location=(37.5, 127.0), max_end_time=10**7, service_duration=service)


# Service time is what makes an unassigned request cost anything.
def request(name: str, service: int = 60) -> DeliveryModel:
    return DeliveryModel(id=name, demand=1, pickup=stop(f"{name}p", service), dropoff=stop(f"{name}d", service))


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


def solve(model_input: PdpInput) -> dict[str, list[str]]:
    solution = solve_request(PdpRequest(model_input=model_input, timelimit=0.3))
    assert solution.feasible
    return {vehicle: [s.stop_id for s in route] for vehicle, route in solution.routes.items()}


def vehicle(name: str, route=(), pinned: int = 0) -> VehicleModel:
    return VehicleModel(id=name, capacity=5, home=(37.5, 127.0), initial_route=list(route), pinned_stops=pinned)


NEARBY = {"v": 0, "ap": 100, "ad": 1000, "bp": 110, "bd": 120}


def test_a_new_request_slips_in_after_the_pinned_stop_and_before_a_free_one():
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=1)], [request("a"), request("b")], NEARBY)

    assert solve(model_input)["v"] == ["ap", "bp", "bd", "ad"]


def test_pinning_both_stops_keeps_new_work_behind_them():
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=2)], [request("a"), request("b")], NEARBY)

    assert solve(model_input)["v"][:2] == ["ap", "ad"]


def test_a_pinned_stop_stays_first_when_something_else_would_be_better():
    places = {"v": 0, "ap": 5000, "ad": 5001, "bp": 1, "bd": 2}
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=1)], [request("a"), request("b")], places)

    assert solve(model_input)["v"][0] == "ap"


def test_a_free_tail_can_be_reordered():
    places = {"v": 0, "ap": 1, "ad": 1000, "bp": 2, "bd": 3}
    route = ["ap", "ad", "bp", "bd"]
    model_input = line_input([vehicle("v", route, pinned=1)], [request("a"), request("b")], places)

    assert solve(model_input)["v"] == ["ap", "bp", "bd", "ad"]


def test_an_unpinned_request_can_move_to_another_vehicle():
    places = {"v": 0, "w": 1000, "ap": 1, "ad": 2, "bp": 1001, "bd": 1002}
    route = ["ap", "ad", "bp", "bd"]
    model_input = line_input([vehicle("v", route, pinned=1), vehicle("w")], [request("a"), request("b")], places)

    routes = solve(model_input)

    assert routes["v"] == ["ap", "ad"]
    assert routes["w"] == ["bp", "bd"]


def test_a_pinned_pickup_keeps_its_dropoff_on_the_vehicle():
    places = {"v": 0, "w": 900, "ap": 0, "ad": 900, "bp": 1, "bd": 2}
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=1), vehicle("w")], [request("a")], places | {"bp": 1})

    assert solve(model_input)["v"] == ["ap", "ad"]


def test_an_initial_route_is_where_the_solve_starts():
    """With no time to search, the given route comes straight back."""
    model_input = line_input([vehicle("v", ["bp", "bd", "ap", "ad"])], [request("a"), request("b")], NEARBY)
    request_model = PdpRequest(
        model_input=model_input, timelimit=0, solver_parameters={"step_count_limit": 1, "ruin_recreate": False}
    )

    solution = solve_request(request_model)

    assert {s.stop_id for s in solution.routes["v"]} == {"ap", "ad", "bp", "bd"}
    assert solution.unassigned == []


def test_a_request_outside_the_initial_routes_is_still_placed():
    model_input = line_input([vehicle("v", ["ap", "ad"])], [request("a"), request("b")], NEARBY)

    assert sorted(solve(model_input)["v"]) == ["ad", "ap", "bd", "bp"]


def test_pinned_stops_score_as_a_hard_violation():
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=2)], [request("a")], NEARBY)
    plan = pdp.models.to_plan(model_input)
    plan.vehicles[0].node_indices = [1, 0]

    assert pdp.constraint_costs(plan)["pinned_stops"]["costs"][0] == 2


def test_constraints_can_be_replaced_with_pins_in_place():
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=1)], [request("a"), request("b")], NEARBY)
    kept = [c for c in get_constraints() if c.name != "minimize_travel_time"]

    solution = solve_request(
        PdpRequest(model_input=model_input, timelimit=0.2), constraints=[*kept, MinimizeTravelTime(weight=2)]
    )

    assert solution.routes["v"][0].stop_id == "ap"


def bound(name: str, vehicle_id: str) -> DeliveryModel:
    return request(name).model_copy(update={"required_vehicle": vehicle_id})


def test_a_required_request_can_change_places_on_its_route():
    places = {"v": 0, "ap": 1, "ad": 1000, "bp": 2, "bd": 3}
    route = ["ap", "ad", "bp", "bd"]
    model_input = line_input([vehicle("v", route, pinned=1)], [bound("a", "v"), bound("b", "v")], places)

    assert solve(model_input)["v"] == ["ap", "bp", "bd", "ad"]


def test_a_required_request_stays_when_another_vehicle_would_be_better():
    places = {"v": 0, "w": 1000, "ap": 1, "ad": 2, "bp": 1001, "bd": 1002}
    route = ["ap", "ad", "bp", "bd"]
    model_input = line_input([vehicle("v", route, pinned=1), vehicle("w")], [request("a"), bound("b", "v")], places)

    routes = solve(model_input)

    assert routes["w"] == []
    assert sorted(routes["v"]) == ["ad", "ap", "bd", "bp"]


def test_a_required_request_counts_as_a_hard_violation_off_its_vehicle():
    places = {"v": 0, "w": 0, "ap": 1, "ad": 2}
    model_input = line_input([vehicle("v"), vehicle("w", ["ap", "ad"])], [bound("a", "v")], places)

    assert sum(pdp.constraint_costs(pdp.models.to_plan(model_input))["required_vehicle"]["costs"]) == 1


def stops_of(solution, vehicle_id: str) -> list[str]:
    return [s.stop_id for s in solution.routes[vehicle_id]]


def construct(model_input: PdpInput):
    return solve_request(PdpRequest(model_input=model_input, timelimit=0))


def test_construction_puts_a_required_request_on_its_vehicle():
    places = {"v": 0, "w": 1000, "ap": 1, "ad": 2}
    model_input = line_input([vehicle("v"), vehicle("w")], [bound("a", "w")], places)

    solution = construct(model_input)

    assert solution.feasible
    assert stops_of(solution, "w") == ["ap", "ad"]


def test_construction_places_a_required_request_before_a_free_one_takes_its_slot():
    def timed(name: str, close: int, required: str | None = None) -> DeliveryModel:
        def timed_stop(suffix: str, end: int) -> StopModel:
            return StopModel(id=f"{name}{suffix}", location=(37.5, 127.0), max_end_time=end, service_duration=1000)

        return DeliveryModel(
            id=name,
            demand=1,
            required_vehicle=required,
            pickup=timed_stop("p", close),
            dropoff=timed_stop("d", close + 2000),
        )

    vehicles = [VehicleModel(id=name, capacity=1, home=(37.5, 127.0)) for name in ("v", "w")]
    places = {"v": 0, "w": 100, "ap": 0, "ad": 0, "bp": 0, "bd": 0}
    # `a` has the tighter window and `v` is closest, but `v` has room for only one of them.
    model_input = line_input(vehicles, [timed("a", 1200), timed("b", 1300, "v")], places)

    solution = construct(model_input)

    assert solution.feasible
    assert stops_of(solution, "v") == ["bp", "bd"]
    assert stops_of(solution, "w") == ["ap", "ad"]


def chained_requests(vehicle_count: int, per_vehicle: int, seed: int) -> PdpInput:
    """Each vehicle has a chain of requests it alone can serve on time, so a solution exists by construction."""
    rng = random.Random(seed)
    positions = {f"v{v}": rng.randrange(20000) for v in range(vehicle_count)}
    deliveries = []
    for v in range(vehicle_count):
        clock, here = 0, positions[f"v{v}"]
        for k in range(per_vehicle):
            name = f"r{v}_{k}"
            pickup = (here + rng.randrange(200, 900)) % 20000
            dropoff = (pickup + rng.randrange(200, 900)) % 20000
            at_pickup = clock + abs(here - pickup)
            at_dropoff = at_pickup + 60 + abs(pickup - dropoff)

            def windowed(stop_id: str, at: int) -> StopModel:
                return StopModel(
                    id=stop_id,
                    location=(37.5, 127.0),
                    min_start_time=max(0, at - 300),
                    max_end_time=at + 300,
                    service_duration=60,
                )

            deliveries.append(
                DeliveryModel(
                    id=name,
                    demand=1,
                    required_vehicle=f"v{v}",
                    pickup=windowed(f"{name}p", at_pickup),
                    dropoff=windowed(f"{name}d", at_dropoff),
                )
            )
            positions[f"{name}p"], positions[f"{name}d"] = pickup, dropoff
            clock, here = at_dropoff + 60, dropoff
    rng.shuffle(deliveries)
    vehicles = [VehicleModel(id=f"v{v}", capacity=2, home=(37.5, 127.0)) for v in range(vehicle_count)]
    return line_input(vehicles, deliveries, positions)


@pytest.mark.parametrize("seed", range(3))
def test_a_problem_of_required_requests_is_feasible_from_construction(seed):
    assert construct(chained_requests(10, 8, seed)).feasible


def test_the_search_takes_a_misplaced_required_request_to_its_vehicle():
    # One request, so nothing is routed near it and the pair move has to pick a vehicle itself.
    names = [f"v{i}" for i in range(40)]
    vehicles = [vehicle("v0", ["ap", "ad"]), *(vehicle(name) for name in names[1:])]
    places = {name: 0 for name in names} | {"ap": 1, "ad": 2}
    model_input = line_input(vehicles, [bound("a", "v39")], places)
    only_the_pair_move = {
        "step_count_limit": 8,
        "pair_assignment_probability": 0.0,
        "relocate_move_weight": 0.0,
        "list_swap_move_weight": 0.0,
        "two_opt_move_weight": 0.0,
        "or_opt_move_weight": 0.0,
        "tail_swap_move_weight": 0.0,
        "ruin_recreate": False,
    }

    for seed in range(5):
        solution = solve_request(
            # The steps end the solve; the time limit is only a safety net.
            PdpRequest(model_input=model_input, timelimit=60, random_seed=seed, solver_parameters=only_the_pair_move)
        )
        assert stops_of(solution, "v39") == ["ap", "ad"]


@pytest.mark.parametrize("plugin_path", [False, True])
def test_a_required_vehicle_outside_the_plan_is_refused_by_the_engine(plugin_path):
    places = {"v": 0, "w": 0, "ap": 1, "ad": 2}
    plan = pdp.models.to_plan(line_input([vehicle("v"), vehicle("w")], [bound("a", "w")], places))
    plan.vehicles = plan.vehicles[:1]
    solver = pdp._core.Solver(pdp._core.SolverConfig())

    with pytest.raises(ValueError, match="requiredVehicleIndex out of range"):
        if plugin_path:
            solver.solve_with_constraints(plan, get_constraints())
        else:
            solver.solve(plan)


def test_an_unknown_required_vehicle_is_refused():
    with pytest.raises(ValueError, match="unknown vehicle"):
        line_input([vehicle("v")], [bound("a", "w")], {"v": 0, "ap": 1, "ad": 2})


@pytest.mark.parametrize(
    "route, pinned, message",
    [
        (["ap", "ad"], 3, "more pinned_stops"),
        (["ap", "nope"], 0, "unknown stop"),
        (["ap"], 0, "needs its pickup"),
        (["ad", "ap"], 0, "needs its pickup"),
        (["ap", "ap"], 0, "appears twice"),
    ],
)
def test_a_bad_initial_route_is_refused(route, pinned, message):
    with pytest.raises(ValueError, match=message):
        line_input([vehicle("v", route, pinned=pinned)], [request("a")], {"v": 0, "ap": 1, "ad": 2})


def test_a_stop_cannot_start_on_two_vehicles():
    places = {"v": 0, "w": 0, "ap": 1, "ad": 2}
    with pytest.raises(ValueError, match="appears twice"):
        line_input([vehicle("v", ["ap", "ad"]), vehicle("w", ["ap", "ad"])], [request("a")], places)
