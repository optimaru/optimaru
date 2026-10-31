"""Starting a solve from an earlier solution."""

import json
import re
import subprocess
import sys

import pytest
import vrp_routing
from vrp_routing.constraints import get_constraints
from vrp_routing.models import (
    RoutePlanRequest,
    RouteSolution,
    RoutingInput,
    VehicleModel,
    VisitModel,
    VisitStop,
    solve_request,
    to_plan,
    with_initial_solution,
)

PLACES = {"v": 0, "w": 0, "a": 100, "b": 200, "c": 300, "d": 400, "e": 500, "f": 600}
VISITS = ["a", "b", "c", "d", "e", "f"]

# Both solve paths. The constraints path is the one that leaves the unassigned
# pool alone, so a visit parked there would never reach construction.
PATHS = [pytest.param(lambda: None, id="default"), pytest.param(get_constraints, id="constraints")]


def problem(timelimit: float = 0, **vehicle_fields) -> RoutePlanRequest:
    """Everything sits on a line. A zero time limit runs construction and no search step."""
    order = ["v", "w", *VISITS]
    return RoutePlanRequest(
        timelimit=timelimit,
        model_input=RoutingInput(
            vehicles=[VehicleModel(id=name, capacity=10, depot=(37.5, 127.0), **vehicle_fields) for name in "vw"],
            visits=[VisitModel(id=name, location=(37.5, 127.0), service_duration=60) for name in VISITS],
            driving_seconds=[[abs(PLACES[a] - PLACES[b]) for b in order] for a in order],
        ),
    )


def solution_of(routes: dict[str, list[str]], unassigned=()) -> RouteSolution:
    stops = {v: [VisitStop(visit_id=i, arrival_time=0, demand=1) for i in route] for v, route in routes.items()}
    return RouteSolution(
        routes=stops, unassigned=list(unassigned), score="0hard/0medium/0soft", feasible=True, total_driving_seconds=0
    )


def routes(solution: RouteSolution) -> dict[str, list[str]]:
    return {vehicle: [stop.visit_id for stop in stops] for vehicle, stops in solution.routes.items()}


def score(solution: RouteSolution) -> tuple[float, ...]:
    return tuple(float(level) for level in re.findall(r"-?\d+", solution.score))


NOT_GREEDY = {"v": ["f", "b", "d"], "w": ["c", "e", "a"]}


@pytest.mark.parametrize("constraints", PATHS)
def test_a_complete_seed_comes_back_unchanged(constraints):
    constructed = routes(solve_request(problem(), constraints=constraints()))
    assert constructed != NOT_GREEDY  # so what comes back cannot have been constructed

    seeded = with_initial_solution(problem(), solution_of(NOT_GREEDY))

    assert routes(solve_request(seeded, constraints=constraints())) == NOT_GREEDY


@pytest.mark.parametrize("as_seed", [lambda s: s, lambda s: json.loads(json.dumps(s.model_dump(mode="json")))])
def test_a_solved_result_feeds_back_as_model_and_as_json(as_seed):
    solved = solve_request(problem(timelimit=0.1))

    seeded = with_initial_solution(problem(), as_seed(solved))

    assert routes(solve_request(seeded)) == routes(solved)


@pytest.mark.parametrize("constraints", PATHS)
def test_a_partial_seed_keeps_its_part_and_fills_the_rest(constraints):
    seeded = with_initial_solution(problem(), solution_of({"v": ["f", "b"]}, unassigned=["a", "c", "d", "e"]))
    assert list(to_plan(seeded.model_input).unassigned_visit_indices) == []

    solution = solve_request(seeded, constraints=constraints())

    assert solution.unassigned == []
    assert sorted(routes(solution)["v"] + routes(solution)["w"]) == VISITS
    assert [visit for visit in routes(solution)["v"] if visit in "fb"] == ["f", "b"]


def test_the_seed_replaces_the_routes_the_problem_had():
    request = with_initial_solution(problem(), solution_of({"v": ["a"], "w": ["b"]}))

    seeded = with_initial_solution(request, solution_of({"v": ["c"]}))

    assert [vehicle.initial_route for vehicle in seeded.model_input.vehicles] == [["c"], []]


@pytest.mark.parametrize(
    "seed, message",
    [
        ({"v": ["a", "nope"]}, "unknown visit 'nope'"),
        ({"nope": ["a"]}, "unknown vehicle .*nope"),
        ({"v": ["a", "a"]}, "'a' appears twice"),
        ({"v": ["a"], "w": ["b", "a"]}, "'a' appears twice"),
    ],
)
def test_a_bad_seed_is_refused(seed, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(problem(), solution_of(seed))


def test_a_dict_that_is_no_solution_is_refused():
    with pytest.raises(ValueError):
        with_initial_solution(problem(), {"routes": "nope"})


def test_a_bad_initial_route_is_refused_on_the_model_too():
    with pytest.raises(ValueError, match="unknown visit 'nope'"):
        problem(initial_route=["nope"])


def test_the_input_is_not_changed():
    request = problem()
    solution = solution_of(NOT_GREEDY)
    before = request.model_dump(), solution.model_dump()

    seeded = with_initial_solution(request, solution)
    seeded.model_input.vehicles[0].initial_route.append("a")
    seeded.model_input.visits[0].demand = 9

    assert (request.model_dump(), solution.model_dump()) == before


@pytest.mark.parametrize("constraints", PATHS)
def test_a_short_search_is_not_worse_than_its_seed(constraints):
    start = solve_request(with_initial_solution(problem(), solution_of(NOT_GREEDY)), constraints=constraints())

    improved = solve_request(
        with_initial_solution(problem(timelimit=0.2), solution_of(NOT_GREEDY)), constraints=constraints()
    )

    assert score(improved) >= score(start)


def test_the_package_exports_the_solve_entry_points():
    assert {"solve_request", "with_initial_solution"} <= set(vrp_routing.__all__)

    seeded = vrp_routing.with_initial_solution(problem(), solution_of(NOT_GREEDY))

    assert routes(vrp_routing.solve_request(seeded)) == NOT_GREEDY


def test_the_package_still_imports_without_pydantic():
    code = "import sys; sys.modules['pydantic'] = None; import vrp_routing; vrp_routing.Solver"

    subprocess.run([sys.executable, "-c", code], check=True)
