import pytest

import optimaru.container_loading as cl
from optimaru.container_loading import with_initial_solution
from optimaru.container_loading.models import ContainerLoadingProblemModel


def _problem() -> ContainerLoadingProblemModel:
    return ContainerLoadingProblemModel(
        containers=[{"id": f"c{c}", "length": 10, "width": 10, "height": 10} for c in range(3)],
        items=[{"id": f"i{i}", "length": 5, "width": 5, "height": 5} for i in range(6)],
    )


def _no_search() -> cl.SolverConfig:
    """Zero budget: the engine stops before its first step."""
    return cl.SolverConfig(stop={"time_limit_seconds": 0})


def _orders(solved) -> dict[str, list[str]]:
    return {container.id: container.items for container in solved.containers}


def test_a_complete_solution_comes_back_unchanged_with_zero_budget():
    plain = cl.solve(_problem(), config=_no_search())
    assert _orders(plain) == {"c0": ["i0", "i1", "i2", "i3", "i4", "i5"], "c1": [], "c2": []}

    solution = {"containers": [{"id": "c2", "items": ["i5", "i4", "i3"]}, {"id": "c0", "items": ["i0", "i1", "i2"]}]}
    seeded = with_initial_solution(_problem(), solution)
    assert [container.initial_items for container in seeded.containers] == [["i0", "i1", "i2"], [], ["i5", "i4", "i3"]]
    solved = cl.solve(seeded, config=_no_search())
    assert _orders(solved) == {"c0": ["i0", "i1", "i2"], "c1": [], "c2": ["i5", "i4", "i3"]}
    assert (solved.score_hard, solved.score_soft) == (0, -2.0)


def test_items_the_solution_leaves_out_are_constructed_around_it():
    seeded = with_initial_solution(_problem(), {"containers": [{"id": "c1", "items": ["i3"]}]})
    solved = cl.solve(seeded, config=_no_search())
    assert _orders(solved) == {"c0": [], "c1": ["i3", "i0", "i1", "i2", "i4", "i5"], "c2": []}


def test_a_solved_model_is_a_solution_and_replaces_earlier_initial_items():
    problem = _problem()
    problem.containers[0].initial_items = ["i5"]
    solved = cl.solve(_problem(), time_limit_seconds=0.2, random_seed=1)
    seeded = with_initial_solution(problem, solved)
    assert [container.initial_items for container in seeded.containers] == [c.items for c in solved.containers]
    assert all(container.items is None and container.loaded_volume is None for container in seeded.containers)
    assert all(item.container is None and item.x is None for item in seeded.items)
    assert seeded.score_hard is None and seeded.unplaced_items is None
    again = cl.solve(seeded, config=_no_search())
    assert _orders(again) == _orders(solved)
    assert with_initial_solution(problem, solved.model_dump(mode="json")) == seeded
    assert problem.containers[0].initial_items == ["i5"]  # the problem passed in is left as it was


def test_an_item_the_solution_could_not_place_is_tried_again():
    problem = ContainerLoadingProblemModel(
        containers=[{"id": "c0", "length": 10, "width": 10, "height": 10}],
        items=[
            {"id": "a", "length": 10, "width": 10, "height": 6, "rotations": "fixed"},
            {"id": "b", "length": 10, "width": 10, "height": 6, "rotations": "fixed"},
        ],
    )
    solved = cl.solve(problem, config=_no_search())
    assert (solved.containers[0].items, solved.containers[0].unplaced_items) == (["a"], ["b"])
    seeded = with_initial_solution(problem, solved)
    assert seeded.containers[0].initial_items == ["a"]
    assert cl.solve(seeded, config=_no_search()).unplaced_items == ["b"]


@pytest.mark.parametrize(
    ("solution", "message"),
    [
        ({"items": []}, "a dict with a 'containers' list"),
        ("c0", "a dict with a 'containers' list"),
        ({"containers": [{"id": "c9", "items": []}]}, "unknown container: 'c9'"),
        ({"containers": [{"items": []}]}, "unknown container: None"),
        ({"containers": [{"id": "c0"}, {"id": "c0"}]}, "lists container 'c0' more than once"),
        ({"containers": [{"id": "c0", "items": ["i9"]}]}, "unknown item 'i9'"),
        ({"containers": [{"id": "c0", "items": ["i1", "i1"]}]}, "lists item 'i1' more than once"),
        ({"containers": [{"id": "c0", "items": ["i1"]}, {"id": "c1", "items": ["i1"]}]}, "item 'i1' more than once"),
        ({"containers": [{"id": "c0", "items": "i1"}]}, "'items' must be a list"),
    ],
)
def test_a_bad_solution_is_refused(solution, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(_problem(), solution)
