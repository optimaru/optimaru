"""solve(): the samples, the engine knobs and the callbacks."""

import json
from pathlib import Path

import pytest

import optimaru.container_loading as cl
from optimaru.container_loading.constraints import (
    UsedLengthCost,
    get_constraints,
)
from optimaru.container_loading.models import ContainerLoadingProblemModel

from .._picks import replace_default

FIXTURES = Path(__file__).parent / "fixtures"


def van_route() -> ContainerLoadingProblemModel:
    return ContainerLoadingProblemModel.model_validate(json.loads((FIXTURES / "van_route.json").read_text()))


def cubes(count: int, containers: int, side: int = 5, space: int = 10) -> cl.ContainerLoadingProblem:
    """`count` cubes for `containers` cubic spaces that hold eight each."""
    return cl.ContainerLoadingProblem(
        [cl.Container(f"c{c}", space, space, space) for c in range(containers)],
        [cl.Item(f"i{i}", side, side, side) for i in range(count)],
    )


def test_the_demo_is_feasible_and_every_box_stands_somewhere():
    problem = cl.generate_demo_data()
    cl.solve(problem, time_limit_seconds=2.0, random_seed=0)
    assert problem.score.is_feasible()
    assert problem.unplaced_items() == []
    assert problem.containers_used() <= 3
    split = sum(cl.constraint_costs(problem)["group_split_cost"]["costs"])
    assert problem.score.soft == -(problem.containers_used() + split)
    assert split <= 2  # three stores on two or three pallets
    assert sorted(i for container in problem.containers for i in container.items) == list(range(40))
    for index, item in enumerate(problem.items):
        container = problem.containers[item.container]
        assert container.placed_items[item.sequence] == index
        assert item.x + item.placed_length <= container.length
        assert item.y + item.placed_width <= container.width
        assert item.z + item.placed_height <= container.height


def test_sixteen_cubes_fill_two_spaces():
    """Eight cubes of 5 fill a space of 10: sixteen need exactly two of the four."""
    problem = cubes(16, 4)
    cl.solve(problem, time_limit_seconds=1.0, random_seed=1)
    assert (problem.score.hard, problem.score.soft) == (0, -2.0)
    used = [container for container in problem.containers if container.placed_items]
    assert [container.fill_ratio for container in used] == [1.0, 1.0]
    assert [container.used_length for container in used] == [10, 10]


def test_an_item_that_fits_nowhere_is_reported_as_unplaced():
    problem = cl.ContainerLoadingProblem(
        [cl.Container("c0", 10, 10, 10)], [cl.Item("big", 11, 5, 5), cl.Item("ok", 5, 5, 5)]
    )
    cl.solve(problem, time_limit_seconds=0.2, random_seed=1)
    assert (problem.score.hard, problem.score.soft) == (-1, -1.0)
    assert problem.unplaced_items() == [0]
    assert not problem.items[0].is_placed() and problem.items[0].container == -1
    assert problem.containers[0].unplaced_items == [0] and problem.containers[0].placed_items == [1]


def test_the_van_is_loaded_last_stop_first():
    """Three stops in one cargo space: nothing may stand between an item and the door, or on
    it, when its stop comes, and nothing stands on the glass."""
    solved = cl.solve(van_route(), time_limit_seconds=2.0, random_seed=0)
    assert (solved.score_hard, solved.score_soft) == (0, -1.0)
    by_id = {item.id: item for item in solved.items}
    assert all(item.container == "van-1" for item in solved.items)
    for a in solved.items:
        for b in solved.items:
            if a.unload_order < b.unload_order:
                same_lane = a.y < b.y + b.placed_width and b.y < a.y + a.placed_width
                same_level = a.z < b.z + b.placed_height and b.z < a.z + a.placed_height
                assert not (b.x >= a.x + a.placed_length and same_lane and same_level), (a.id, b.id)
    glass = by_id["stop1-glass"]
    assert not any(
        other.z == glass.z + glass.placed_height
        and other.x < glass.x + glass.placed_length
        and glass.x < other.x + other.placed_length
        and other.y < glass.y + glass.placed_width
        and glass.y < other.y + other.placed_width
        for other in solved.items
    )


def test_a_truck_with_many_stops_comes_out_in_stop_order():
    """Twelve stops, 96 cartons of four sizes drawn at random, a third of the cargo space: the
    construction alone loads every stop clear of the later ones, and a short search keeps it."""
    import random

    rng = random.Random(4)
    sizes = [(600, 400, 400), (400, 300, 300), (500, 400, 300), (300, 200, 200)]
    items = []
    for i in range(96):
        length, width, height = rng.choice(sizes)
        items.append(
            cl.Item(
                f"p{i}",
                length,
                width,
                height,
                weight=rng.randint(1000, 15000),
                rotations=cl.Rotation.UPRIGHT,
                unload_order=1 + rng.randrange(12),
            )
        )
    truck = cl.Container("truck", 4300, 1800, 1900)
    zero = cl.SolverConfig(stop={"time_limit_seconds": 0})
    constructed = cl.solve(cl.ContainerLoadingProblem([truck], items), config=zero)
    assert constructed.score.hard == 0
    problem = cl.ContainerLoadingProblem([truck], items)
    cl.solve(problem, config=cl.SolverConfig(stop={"step_limit": 2000}, run={"random_seed": 1}))
    assert problem.score.hard == 0 and problem.unplaced_items() == []


def test_nearby_selection_by_stop_can_be_switched_off():
    """The setting reaches the engine: 0 turns nearby selection off, and the same seeded solve then
    takes another path through the same truck (other improvements, at other steps)."""
    import random

    rng = random.Random(7)
    sizes = [(600, 400, 400), (400, 300, 300), (500, 400, 300), (300, 200, 200)]
    items = [
        cl.Item(f"p{i}", *rng.choice(sizes), unload_order=1 + rng.randrange(8), rotations=cl.Rotation.UPRIGHT)
        for i in range(40)
    ]
    truck = [cl.Container("truck", 3000, 1500, 1500)]
    picks = replace_default(get_constraints(), "used_length_cost", UsedLengthCost(weight=1.0))
    paths = []
    for nearby in (0.8, 0.0):
        config = cl.SolverConfig(stop={"step_limit": 3000}, run={"random_seed": 3})
        config.selection.nearby_probability = nearby
        assert config.selection.nearby_probability == nearby
        path = []
        cl.solve(
            cl.ContainerLoadingProblem(truck, items),
            config=config,
            constraints=picks,
            on_new_best=lambda step, score, path=path: path.append((step, score.hard, score.soft)),
        )
        paths.append(path)
    assert paths[0] != paths[1]


def test_the_cheapest_cartons_that_hold_an_order_are_chosen():
    """A catalog of three carton sizes, two copies each: the order fits the medium one, which
    costs less than the large one and less than two small ones."""
    catalog = {"small": (300, 200, 200, 2.0), "medium": (400, 300, 300, 3.0), "large": (600, 400, 400, 5.0)}
    containers = [
        cl.Container(f"{name}-{copy}", length, width, height, cost=cost)
        for name, (length, width, height, cost) in catalog.items()
        for copy in (1, 2)
    ]
    items = [cl.Item(f"box-{i}", 200, 150, 150, weight=500, group="order-1") for i in range(8)]
    problem = cl.ContainerLoadingProblem(containers, items)
    cl.solve(problem, time_limit_seconds=2.0, random_seed=0)
    assert (problem.score.hard, problem.score.soft) == (0, -3.0)
    used = [container.id for container in problem.containers if container.placed_items]
    assert len(used) == 1 and used[0].startswith("medium")


def test_on_new_best_reports_step_and_score():
    seen = []
    problem = cl.generate_demo_data()
    cl.solve(problem, time_limit_seconds=1.0, random_seed=3, on_new_best=lambda step, score: seen.append((step, score)))
    assert seen
    assert all(isinstance(step, int) and isinstance(score, cl.Score) for step, score in seen)
    levels = [(score.hard, score.soft) for _, score in seen]
    assert levels == sorted(levels)
    assert levels[-1] == (problem.score.hard, problem.score.soft)


def test_stop_at_first_feasible_stops_early():
    problem = cl.generate_demo_data()
    cl.solve(problem, time_limit_seconds=30.0, random_seed=3, stop_at_first_feasible=True)
    assert problem.score.hard == 0


def test_a_step_limit_makes_the_search_deterministic():
    def run():
        problem = cl.generate_demo_data()
        config = cl.SolverConfig()
        config.stop.step_limit = 3000
        config.run.random_seed = 9
        cl.solve(problem, config=config)
        return problem.loading(), (problem.score.hard, problem.score.soft)

    assert run() == run()


def test_a_zero_budget_returns_the_construction():
    """Largest first, each into the first container that takes it."""
    problem = cubes(10, 3)
    config = cl.SolverConfig(stop={"time_limit_seconds": 0})
    cl.solve(problem, config=config)
    assert problem.loading() == [list(range(8)), [8, 9], []]
    assert (problem.score.hard, problem.score.soft) == (0, -2.0)


def test_the_lists_a_problem_comes_with_are_the_starting_point():
    problem = cubes(6, 3)
    problem.set_loading([[5], [], [4, 3]])
    cl.solve(problem, config=cl.SolverConfig(stop={"time_limit_seconds": 0}))
    assert problem.loading() == [[5, 0, 1, 2], [], [4, 3]]


def test_without_first_fit_decreasing_the_engine_inserts():
    problem = cubes(16, 4)
    config = cl.SolverConfig(construction={"first_fit_decreasing": False}, stop={"time_limit_seconds": 1.0})
    cl.solve(problem, config=config)
    assert problem.score.hard == 0
    assert sorted(i for order in problem.loading() for i in order) == list(range(16))


@pytest.mark.parametrize("move", ["empty_container", "repack", "switch_container", "stop_order"])
def test_the_custom_moves_can_be_switched_off(move):
    problem = cl.generate_demo_data()
    config = cl.SolverConfig(stop={"step_limit": 2000}, run={"random_seed": 1})
    setattr(config.custom_moves, move, False)
    cl.solve(problem, config=config)
    assert problem.score.hard == 0


def test_the_domain_moves_close_containers():
    """Twelve small boxes spread over six spaces by hand fit one. With EmptyContainerMove or
    RepackPairMove the search packs them into it; with both off, 300 steps of single-item moves
    do not."""

    def solve(**moves: bool) -> float:
        problem = cubes(12, 6, side=2)
        problem.set_loading([[i, i + 6] for i in range(6)])
        config = cl.SolverConfig(stop={"step_limit": 300}, run={"random_seed": 2}, alns_trigger={"enabled": False})
        config.custom_moves = moves
        cl.solve(problem, config=config)
        assert problem.score.hard == 0
        return problem.score.soft

    assert solve(empty_container=True, repack=False) == -1.0
    assert solve(empty_container=False, repack=True) == -1.0
    assert solve(empty_container=False, repack=False) < -1.0


def test_empty_container_candidates_must_be_positive():
    config = cl.SolverConfig()
    with pytest.raises(ValueError, match="custom_moves.empty_container_candidates must be positive"):
        config.custom_moves.empty_container_candidates = 0
    config.custom_moves.empty_container_candidates = 1
    assert config.custom_moves.empty_container_candidates == 1


def test_full_assert_solves_without_a_score_mismatch():
    problem = cl.generate_demo_data(item_count=24, container_count=3)
    config = cl.SolverConfig(stop={"step_limit": 1500}, run={"random_seed": 5})
    cl.solve(problem, config=config, full_assert=True)
    assert problem.score.hard == 0


def test_the_constraints_path_scores_with_the_picks():
    """used_length_cost at a weight: the soft score carries the loading length, and the
    search packs towards the closed end."""
    picks = replace_default(get_constraints(), "used_length_cost", UsedLengthCost(weight=0.01))
    problem = cubes(4, 1, side=2, space=10)
    cl.solve(problem, time_limit_seconds=0.5, random_seed=1, constraints=picks)
    assert problem.containers[0].used_length == 2  # one wall of four at the closed end
    assert (problem.score.hard, problem.score.soft) == (0, pytest.approx(-(1.0 + 0.02)))
    assert cl.calculate_score(problem).soft == -1.0  # the defaults leave the length out


def test_parallel_starts_are_ignored_with_constraints():
    problem = cl.generate_demo_data()
    config = cl.SolverConfig(stop={"step_limit": 500}, run={"random_seed": 1, "parallel_solver_count": 3})
    single = cl.generate_demo_data()
    cl.solve(problem, config=config, constraints=get_constraints())
    config.run.parallel_solver_count = 1
    cl.solve(single, config=config, constraints=get_constraints())
    assert problem.loading() == single.loading()


def test_solve_refuses_other_types():
    with pytest.raises(TypeError, match="ContainerLoadingProblem or a models.ContainerLoadingProblemModel"):
        cl.solve({"containers": [], "items": []})


def test_an_empty_problem_scores_zero():
    problem = cl.ContainerLoadingProblem([cl.Container("c", 1, 1, 1)], [])
    cl.solve(problem, time_limit_seconds=0.1)
    assert (problem.score.hard, problem.score.soft) == (0, 0.0)
    assert problem.containers_used() == 0 and problem.unplaced_items() == []


@pytest.mark.parametrize(
    ("build", "message"),
    [
        (lambda: cl.ContainerLoadingProblem([], [cl.Item("a", 1, 1, 1)]), "at least one container"),
        (lambda: cl.ContainerLoadingProblem([cl.Container("c", 0, 1, 1)], []), "between 1 and 1000000"),
        (lambda: cl.ContainerLoadingProblem([cl.Container("c", 1, 1, 1)], [cl.Item("a", 1, 1, -1)]), "between 1 and"),
        (
            lambda: cl.ContainerLoadingProblem([cl.Container("c", 1, 1, 1)], [cl.Item("a", 1, 1, 1, weight=-1)]),
            "weight must be between 0 and",
        ),
        (
            lambda: cl.ContainerLoadingProblem([cl.Container("c", 1, 1, 1), cl.Container("c", 1, 1, 1)], []),
            "container ids must be unique",
        ),
        (
            lambda: cl.ContainerLoadingProblem([cl.Container("c", 1, 1, 1)], [cl.Item("a", 1, 1, 1)] * 2),
            "item ids must be unique",
        ),
        (
            lambda: cl.ContainerLoadingProblem([cl.Container("c", 1, 1, 1, items=[0, 0])], [cl.Item("a", 1, 1, 1)]),
            "more than one place",
        ),
        (
            lambda: cl.ContainerLoadingProblem([cl.Container("c", 1, 1, 1)], [], min_support_ratio=0.0),
            "min_support_ratio must be above 0 and at most 1",
        ),
        (lambda: cl.ContainerLoadingProblem([cl.Container("c", 1, 1, 1, cost=-1.0)], []), "cost must be finite"),
    ],
)
def test_a_bad_problem_is_refused_with_a_value_error(build, message):
    with pytest.raises(ValueError, match=message):
        build()


def test_set_loading_checks_its_lists():
    problem = cubes(3, 2)
    with pytest.raises(ValueError, match="one list per container"):
        problem.set_loading([[0, 1, 2]])
    with pytest.raises(ValueError, match="out of range"):
        problem.set_loading([[0, 7], []])
    with pytest.raises(ValueError, match="more than once"):
        problem.set_loading([[0, 1], [1]])
    problem.set_loading([[2], [0]])
    assert problem.loading() == [[2], [0]]
    cl.calculate_score(problem)
    assert problem.unplaced_items() == [1]  # in no list: not placed, and it costs nothing
    assert problem.score.hard == 0
