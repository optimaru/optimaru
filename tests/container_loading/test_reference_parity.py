"""What the engine returns is a loading, and its costs are the ones the definitions give.

`reference_container_loading` checks a solved model without the decoder: the positions
are a valid loading (inside, no overlap, enough support, an allowed orientation), and
every constraint's raw cost recomputed from those positions equals `constraint_costs()`.
"""

from __future__ import annotations

import json
import random
from pathlib import Path

import pytest

import optimaru.container_loading as cl
from optimaru.container_loading.constraints import score
from optimaru.container_loading.models import ContainerLoadingProblemModel as Model

from . import reference_container_loading as ref

FIXTURES = Path(__file__).parent / "fixtures"


def random_problem(seed: int) -> cl.ContainerLoadingProblem:
    """Boxes with sides of 1 to 6 for a few spaces of two sizes, with every optional field in use
    (stacking rules, layer limits, bottom_only, stack heights, balance limits, priorities,
    optional items and group rules included)."""
    rng = random.Random(seed)
    containers = [
        cl.Container(
            f"c{c}",
            12 if c % 2 == 0 else 9,
            8,
            9,
            max_weight=rng.choice([None, 40, 60]),
            cost=rng.choice([1.0, 2.0, 2.5]),
            max_stack_height=rng.choice([None, None, 7]),
            max_offset_length=rng.choice([None, None, 0.1, 0.3]),
            max_offset_width=rng.choice([None, None, 0.2]),
        )
        for c in range(rng.randint(2, 5))
    ]
    items = [
        cl.Item(
            f"i{i}",
            rng.randint(1, 6),
            rng.randint(1, 6),
            rng.randint(1, 6),
            weight=rng.randint(0, 9),
            rotations=rng.choice(list(cl.Rotation.__members__.values())),
            max_load_on_top=rng.choice([None, None, 0, 3, 8]),
            unload_order=rng.choice([None, 1, 2, 3]),
            group=rng.choice([None, "a", "b", "c"]),
            bottom_only=rng.random() < 0.1,
            max_stack_layers=rng.choice([None, None, None, 1, 2, 3]),
            stack_group=rng.choice([None, "food", "chem"]),
            priority=rng.choice([1, 1, 2, 5]),
            optional=rng.random() < 0.25,
        )
        for i in range(rng.randint(8, 40))
    ]
    rules = rng.choice([[], [("chem", "food")], [("chem", "food"), ("food", "food")]])
    groups = sorted({item.group for item in items if item.group is not None})
    group_rules = [(group, rng.random() < 0.5, rng.random() < 0.5) for group in groups if rng.random() < 0.7]
    return cl.ContainerLoadingProblem(
        containers,
        items,
        min_support_ratio=rng.choice([0.5, 0.8, 1.0]),
        stacking_rules=rules,
        group_rules=group_rules,
    )


def scatter(problem: cl.ContainerLoadingProblem, rng: random.Random) -> None:
    """Every item into a random list at a random place: a loading no search has tidied."""
    lists: list[list[int]] = [[] for _ in problem.containers]
    for item in range(len(problem.items)):
        target = rng.choice(lists)
        target.insert(rng.randint(0, len(target)), item)
    problem.set_loading(lists)


def assert_parity(problem: cl.ContainerLoadingProblem) -> None:
    cl.calculate_score(problem)  # the default score, also for a loading no solve has scored
    model = Model.from_core(problem)
    assert ref.loading_faults(model) == []
    engine = cl.constraint_costs(problem)
    expected = ref.raw_costs(model)
    assert {name: entry["costs"] for name, entry in engine.items()} == expected
    assert score(engine) == (model.score_hard, model.score_soft)


@pytest.mark.parametrize("seed", range(40))
def test_random_loadings_are_valid_and_cost_what_the_reference_says(seed):
    problem = random_problem(seed)
    scatter(problem, random.Random(seed))
    assert_parity(problem)


@pytest.mark.parametrize("seed", range(12))
def test_solved_problems_are_valid_and_cost_what_the_reference_says(seed):
    problem = random_problem(100 + seed)
    config = cl.SolverConfig(stop={"time_limit_seconds": 30, "step_limit": 1500}, run={"random_seed": seed})
    cl.solve(problem, config=config)
    assert_parity(problem)


def test_the_demo_and_the_fixture_are_valid_loadings():
    demo = cl.generate_demo_data()
    cl.solve(demo, time_limit_seconds=1.0, random_seed=0)
    assert demo.score.hard == 0
    assert_parity(demo)

    van = Model.model_validate(json.loads((FIXTURES / "van_route.json").read_text())).to_core()
    cl.solve(van, time_limit_seconds=1.0, random_seed=0)
    assert van.score.hard == 0
    assert_parity(van)


def test_the_reference_notices_a_bad_loading():
    """The checker is not vacuous: a moved box is an overlap, a lifted one is unsupported."""
    problem = cl.ContainerLoadingProblem(
        [cl.Container("c", 10, 10, 10)], [cl.Item("a", 5, 5, 5, rotations=cl.Rotation.FIXED) for _ in range(1)]
    )
    problem.items = [cl.Item("a", 5, 5, 5), cl.Item("b", 5, 5, 5)]
    problem.set_loading([[0, 1]])
    model = Model.from_core(problem)
    assert ref.loading_faults(model) == []
    moved = model.model_copy(deep=True)
    moved.items[1].y = 2
    assert any("overlaps" in fault for fault in ref.loading_faults(moved))
    lifted = model.model_copy(deep=True)
    lifted.items[1].z = 3
    assert any("supported" in fault for fault in ref.loading_faults(lifted))
    outside = model.model_copy(deep=True)
    outside.items[1].x = 8
    assert any("outside" in fault for fault in ref.loading_faults(outside))
