"""Starting a solve from an earlier solution.

`test_initial_routes.py` covers `initial_route` itself; this is the step before
it, turning a finished solution back into initial routes.
"""

import json
import re

import pdp_routing as pdp
import pytest
from pdp_routing.constraints import get_constraints
from pdp_routing.models import (
    DeliveryModel,
    PdpInput,
    PdpRequest,
    PdpSolution,
    PdpStop,
    StopModel,
    VehicleModel,
    solve_request,
    with_initial_solution,
)

PLACES = {"v": 0, "w": 0, "ap": 100, "ad": 200, "bp": 300, "bd": 400, "cp": 500, "cd": 600}
STOPS = sorted(PLACES.keys() - {"v", "w"})
NOT_GREEDY = {"v": ["cp", "ap", "cd", "ad"], "w": ["bp", "bd"]}

# max_stops left at the JSON default would send both through the constraints path.
PATHS = [pytest.param(lambda: None, id="default"), pytest.param(get_constraints, id="constraints")]


def stop(name: str) -> StopModel:
    return StopModel(id=name, location=(37.5, 127.0), max_end_time=10**7, service_duration=60)


def problem(timelimit: float = 0, *, routes=None, pinned: int = 0) -> PdpRequest:
    """Everything sits on a line. A zero time limit runs construction and no search step."""
    routes = routes or {}
    vehicles = [
        VehicleModel(
            id=name,
            capacity=5,
            home=(37.5, 127.0),
            max_stops=0,
            initial_route=routes.get(name, []),
            pinned_stops=pinned if name == "v" else 0,
        )
        for name in "vw"
    ]
    deliveries = [DeliveryModel(id=name, demand=1, pickup=stop(f"{name}p"), dropoff=stop(f"{name}d")) for name in "abc"]
    order = ["v", "w", "ap", "ad", "bp", "bd", "cp", "cd"]
    return PdpRequest(
        timelimit=timelimit,
        model_input=PdpInput(
            vehicles=vehicles,
            deliveries=deliveries,
            driving_seconds=[[abs(PLACES[a] - PLACES[b]) for b in order] for a in order],
        ),
    )


def solution_of(routes: dict[str, list[str]], unassigned=()) -> PdpSolution:
    def planned(name: str) -> PdpStop:
        return PdpStop(stop_id=name, delivery_id=name[0], pickup=name.endswith("p"), arrival_time=0)

    return PdpSolution(
        routes={vehicle: [planned(name) for name in route] for vehicle, route in routes.items()},
        unassigned=list(unassigned),
        score="0hard/0medium/0soft",
        feasible=True,
        total_driving_seconds=0,
    )


def routes_of(solution: PdpSolution) -> dict[str, list[str]]:
    return {vehicle: [stop.stop_id for stop in stops] for vehicle, stops in solution.routes.items()}


def score(solution: PdpSolution) -> tuple[float, ...]:
    return tuple(float(level) for level in re.findall(r"-?\d+", solution.score))


@pytest.mark.parametrize("constraints", PATHS)
def test_a_complete_seed_comes_back_unchanged(constraints):
    constructed = routes_of(solve_request(problem(), constraints=constraints()))
    assert constructed != NOT_GREEDY  # so what comes back cannot have been constructed

    seeded = with_initial_solution(problem(), solution_of(NOT_GREEDY))

    assert routes_of(solve_request(seeded, constraints=constraints())) == NOT_GREEDY


@pytest.mark.parametrize("as_seed", [lambda s: s, lambda s: json.loads(json.dumps(s.model_dump(mode="json")))])
def test_a_solved_result_feeds_back_as_model_and_as_json(as_seed):
    solved = solve_request(problem(timelimit=0.1))

    seeded = with_initial_solution(problem(), as_seed(solved))

    assert routes_of(solve_request(seeded)) == routes_of(solved)


@pytest.mark.parametrize("constraints", PATHS)
def test_a_partial_seed_keeps_its_part_and_fills_the_rest(constraints):
    seed = solution_of({"v": ["cp", "ap", "cd", "ad"]}, unassigned=["b"])

    solution = solve_request(with_initial_solution(problem(), seed), constraints=constraints())

    assert solution.unassigned == []
    assert sorted(routes_of(solution)["v"] + routes_of(solution)["w"]) == STOPS
    assert [name for name in routes_of(solution)["v"] if name[0] in "ac"] == ["cp", "ap", "cd", "ad"]


def test_the_seed_replaces_the_routes_the_problem_had():
    request = problem(routes={"v": ["ap", "ad"], "w": ["bp", "bd"]})

    seeded = with_initial_solution(request, solution_of({"v": ["cp", "cd"]}))

    assert [vehicle.initial_route for vehicle in seeded.model_input.vehicles] == [["cp", "cd"], []]


def test_a_pinned_prefix_carries_over_and_stays_pinned():
    request = problem(routes={"v": ["ap", "ad"]}, pinned=1)

    seeded = with_initial_solution(request, solution_of({"v": ["ap", "cp", "cd", "ad"], "w": ["bp", "bd"]}))

    assert seeded.model_input.vehicles[0].pinned_stops == 1
    assert routes_of(solve_request(seeded))["v"][0] == "ap"


@pytest.mark.parametrize(
    "seed",
    [
        {"v": ["bp", "ap", "ad", "bd"]},  # the pinned stop is no longer first
        {"w": ["ap", "ad"]},  # it moved to another vehicle
        {},  # it was left out
    ],
)
def test_a_seed_must_start_with_the_pinned_stops(seed):
    with pytest.raises(ValueError, match=r"vehicle v: .*pinned stops \['ap'\]"):
        with_initial_solution(problem(routes={"v": ["ap", "ad"]}, pinned=1), solution_of(seed))


@pytest.mark.parametrize(
    "seed, message",
    [
        ({"v": ["ap", "nope"]}, "unknown stop"),
        ({"nope": ["ap", "ad"]}, "unknown vehicle .*nope"),
        ({"v": ["ap", "ap", "ad"]}, "appears twice"),
        ({"v": ["ap", "ad"], "w": ["ap", "ad"]}, "appears twice"),
        ({"v": ["ad", "ap"]}, "request a needs its pickup"),  # a broken pair: reported, not repaired
        ({"v": ["ap"], "w": ["ad"]}, "request a needs its pickup"),
    ],
)
def test_a_bad_seed_is_refused(seed, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(problem(), solution_of(seed))


def test_a_dict_that_is_no_solution_is_refused():
    with pytest.raises(ValueError):
        with_initial_solution(problem(), {"routes": "nope"})


def test_the_input_is_not_changed():
    request = problem(routes={"v": ["ap", "ad"]})
    solution = solution_of(NOT_GREEDY)
    before = request.model_dump(), solution.model_dump()

    seeded = with_initial_solution(request, solution)
    seeded.model_input.vehicles[0].initial_route.append("bp")
    seeded.model_input.deliveries[0].demand = 9

    assert (request.model_dump(), solution.model_dump()) == before


@pytest.mark.parametrize("constraints", PATHS)
def test_a_short_search_is_not_worse_than_its_seed(constraints):
    start = solve_request(with_initial_solution(problem(), solution_of(NOT_GREEDY)), constraints=constraints())

    improved = solve_request(
        with_initial_solution(problem(timelimit=0.2), solution_of(NOT_GREEDY)), constraints=constraints()
    )

    assert score(improved) >= score(start)


def test_the_package_exports_it():
    assert "with_initial_solution" in pdp.__all__
    assert pdp.with_initial_solution is with_initial_solution
