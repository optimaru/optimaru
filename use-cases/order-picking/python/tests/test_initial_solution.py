"""Starting a solve from an earlier solution."""

import json
import re
import subprocess
import sys

import pytest

import order_picking as op
from order_picking.constraints import get_constraints
from order_picking.models import (
    LocationModel,
    OrderModel,
    OrderPickingRequest,
    OrderPickingResponse,
    PickStop,
    ProductModel,
    TrolleyModel,
    solve_request,
    with_initial_solution,
)

PATHS = [pytest.param(lambda: None, id="default"), pytest.param(get_constraints, id="constraints")]
TASKS = ["first-0", "first-1", "first-2", "second-0", "second-1", "second-2"]
NOT_GREEDY = {"1": ["second-2", "first-0", "second-1"], "2": ["first-2", "second-0", "first-1"]}


def spot(column: str, row: int) -> LocationModel:
    return LocationModel(column=column, shelving_row=1, side="LEFT", row=row)


def problem(timelimit: float = 0) -> OrderPickingRequest:
    """A zero time limit runs construction and no search step."""
    spots = [("B", 2), ("C", 3), ("D", 1), ("D", 2), ("E", 1), ("A", 3)]
    items = [ProductModel(id=f"p{i}", volume=1000, location=spot(*place)) for i, place in enumerate(spots)]
    return OrderPickingRequest(
        timelimit=timelimit,
        trolleys=[
            TrolleyModel(id=name, bucket_count=4, bucket_capacity=48000, location=spot("A", 0)) for name in ("1", "2")
        ],
        orders=[OrderModel(id="first", items=items[:3]), OrderModel(id="second", items=items[3:])],
    )


def solution_of(routes: dict[str, list[str]]) -> OrderPickingResponse:
    def stop(task_id: str) -> PickStop:
        return PickStop(
            pick_task_id=task_id, order_id="any", product_name="any", volume=1, location=spot("A", 0)
        )

    return OrderPickingResponse(
        routes={trolley: [stop(task_id) for task_id in tasks] for trolley, tasks in routes.items()},
        distance_by_trolley={},
        score="0hard/0soft",
        feasible=True,
        total_distance=0,
        order_split_count=0,
    )


def routes(solution: OrderPickingResponse) -> dict[str, list[str]]:
    return {trolley: [stop.pick_task_id for stop in stops] for trolley, stops in solution.routes.items()}


def score(solution: OrderPickingResponse) -> tuple[float, ...]:
    return tuple(float(level) for level in re.findall(r"-?\d+", solution.score))


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
    seeded = with_initial_solution(problem(), solution_of({"1": ["second-2", "first-0"]}))

    solution = solve_request(seeded, constraints=constraints())

    assert solution.unassigned == []
    assert sorted(routes(solution)["1"] + routes(solution)["2"]) == TASKS
    assert [task for task in routes(solution)["1"] if task in ("second-2", "first-0")] == ["second-2", "first-0"]


def test_the_seed_replaces_the_routes_the_problem_had():
    request = with_initial_solution(problem(), solution_of({"1": ["first-0"], "2": ["first-1"]}))

    seeded = with_initial_solution(request, solution_of({"1": ["first-2"]}))

    assert [trolley.initial_route for trolley in seeded.trolleys] == [["first-2"], []]


@pytest.mark.parametrize(
    "seed, message",
    [
        ({"1": ["first-0", "first-9"]}, "unknown pick task 'first-9'"),
        ({"nope": ["first-0"]}, "unknown trolley .*nope"),
        ({"1": ["first-0", "first-0"]}, "'first-0' appears twice"),
        ({"1": ["first-0"], "2": ["first-1", "first-0"]}, "'first-0' appears twice"),
    ],
)
def test_a_bad_seed_is_refused(seed, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(problem(), solution_of(seed))


def test_a_dict_that_is_no_solution_is_refused():
    with pytest.raises(ValueError):
        with_initial_solution(problem(), {"routes": "nope"})


def test_the_input_is_not_changed():
    request = problem()
    solution = solution_of(NOT_GREEDY)
    before = request.model_dump(), solution.model_dump()

    seeded = with_initial_solution(request, solution)
    seeded.trolleys[0].initial_route.append("first-0")
    seeded.orders[0].items[0].volume = 9

    assert (request.model_dump(), solution.model_dump()) == before


@pytest.mark.parametrize("constraints", PATHS)
def test_a_short_search_is_not_worse_than_its_seed(constraints):
    start = solve_request(with_initial_solution(problem(), solution_of(NOT_GREEDY)), constraints=constraints())

    improved = solve_request(
        with_initial_solution(problem(timelimit=0.2), solution_of(NOT_GREEDY)), constraints=constraints()
    )

    assert score(improved) >= score(start)


def test_the_package_exports_it():
    assert "with_initial_solution" in op.__all__

    seeded = op.with_initial_solution(problem(), solution_of(NOT_GREEDY))

    assert routes(solve_request(seeded)) == NOT_GREEDY


def test_the_package_still_imports_without_pydantic():
    code = "import sys; sys.modules['pydantic'] = None; import order_picking; order_picking.Solver"

    subprocess.run([sys.executable, "-c", code], check=True)
