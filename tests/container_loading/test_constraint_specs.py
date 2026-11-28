"""optimaru.container_loading.constraints — an API for picking the cost function declaratively.

"Which constraints are on", the catalog, the five knobs (the level included) and repeated
picks are covered here; what every use case's specs share is in tests/test_constraints_contract.py.
"""

import math
import random

import pytest

import optimaru.container_loading as cl
from optimaru.container_loading.constraints import (
    ContainerWeightCost,
    GroupSplitCost,
    Level,
    LoadBalanceCost,
    StackingLoadCost,
    UnloadingOrderCost,
    UnplacedItemCost,
    UsedContainerCost,
    UsedLengthCost,
    container_weight_cost,
    get_constraints,
    group_split_cost,
    load_balance_cost,
    score,
    stacking_load_cost,
    unloading_order_cost,
    unplaced_item_cost,
    used_container_cost,
    used_length_cost,
)

from .._picks import only, replace_default, without

NAMES = [
    "unplaced_item_cost",
    "container_weight_cost",
    "stacking_load_cost",
    "unloading_order_cost",
    "used_container_cost",
    "used_length_cost",
    "load_balance_cost",
    "group_split_cost",
]
PER_CONTAINER = NAMES[:-1]  # one raw cost per container; group_split_cost is the one global
CLASSES = (
    UnplacedItemCost,
    ContainerWeightCost,
    StackingLoadCost,
    UnloadingOrderCost,
    UsedContainerCost,
    UsedLengthCost,
    LoadBalanceCost,
    GroupSplitCost,
)

# The balance of hand()'s two loads, in thousandths of the half-diagonal sqrt(29) of a 10 x 4 floor:
# c0's centre of gravity is at x = (8 * 2 + 7 * 6) / 15, c1's at x = 2; both are centred across.
BALANCE = [
    math.floor(1000 * (5 - 58 / 15) / math.sqrt(29) + 0.5),
    math.floor(1000 * 3 / math.sqrt(29) + 0.5),
]


def hand() -> cl.ContainerLoadingProblem:
    """Two spaces of 10 x 4 x 4, loaded by hand.

    c0 (cost 2, carries 10): a (weight 8, stop 1, carries 5) then b (weight 7, stop 2) in front of
    it. c1: c (weight 1) and d, which is too large for it. a and c are one group.

    Raw: unplaced [0, 1], weight [5, 0], stacking [0, 0], unloading [1, 0], containers [2, 1],
    length [8, 4], balance BALANCE, one group split."""
    fixed = cl.Rotation.FIXED
    problem = cl.ContainerLoadingProblem(
        [cl.Container("c0", 10, 4, 4, max_weight=10, cost=2.0), cl.Container("c1", 10, 4, 4)],
        [
            cl.Item("a", 4, 4, 4, weight=8, rotations=fixed, unload_order=1, group="order", max_load_on_top=5),
            cl.Item("b", 4, 4, 4, weight=7, rotations=fixed, unload_order=2),
            cl.Item("c", 4, 4, 4, weight=1, rotations=fixed, group="order"),
            cl.Item("d", 5, 5, 5, rotations=fixed),
        ],
    )
    problem.set_loading([[0, 1], [2, 3]])
    return problem


def engine_score(problem, picks):
    """Score the loading `problem` holds under `picks` on the engine: no step is taken, and the
    lists a problem comes with are kept."""
    loading = problem.loading()
    cl.Solver(cl.SolverConfig(stop={"time_limit_seconds": 0})).solve_with_constraints(problem, picks)
    assert problem.loading() == loading
    return problem.score.hard, problem.score.soft


def solved(constraints=None) -> cl.ContainerLoadingProblem:
    problem = cl.generate_demo_data(item_count=20, container_count=3)
    config = cl.SolverConfig(stop={"step_limit": 800}, run={"random_seed": 1})
    cl.solve(problem, config=config, constraints=constraints)
    return problem


def test_list_constraints_matches_the_spec_names():
    assert cl.list_constraints() == NAMES
    assert [spec.name for spec in get_constraints()] == NAMES
    assert get_constraints() == [
        unplaced_item_cost,
        container_weight_cost,
        stacking_load_cost,
        unloading_order_cost,
        used_container_cost,
        used_length_cost,
        load_balance_cost,
        group_split_cost,
    ]
    assert [type(spec) for spec in get_constraints()] == list(CLASSES)


def test_every_default_instance_is_exported():
    import optimaru.container_loading.constraints as module

    for name, spec in zip(NAMES, CLASSES):
        assert name in module.__all__ and spec.__name__ in module.__all__
        assert getattr(module, name).name == name
        assert (getattr(module, name).weight, getattr(module, name).level, getattr(module, name).exponent) == (
            None,
            None,
            1.0,
        )
    assert module.__all__ == sorted(module.__all__)


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = cl.constraint_info()
    assert [row.name for row in rows] == NAMES
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("soft", 1.0, True, False, True),
        ("soft", 0.0, True, False, False),
        ("soft", 0.0, True, False, False),
        ("soft", 1.0, True, False, True),
    ]


def test_constraint_costs_are_the_raw_costs_by_hand():
    raw = cl.constraint_costs(hand())
    assert list(raw) == NAMES
    assert {name: entry["costs"] for name, entry in raw.items()} == {
        "unplaced_item_cost": [0, 1],
        "container_weight_cost": [5, 0],
        "stacking_load_cost": [0, 0],
        "unloading_order_cost": [1, 0],
        "used_container_cost": [2, 1],
        "used_length_cost": [8, 4],
        "load_balance_cost": BALANCE,
        "group_split_cost": [1],
    }
    for row in cl.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"]) == (row.default_weight, row.default_level)
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] is (row.name == "group_split_cost")
        assert (entry["name"], entry["label"]) == (row.name, "")
        assert "medium_fold" not in entry  # no medium level in this domain


def test_the_default_score_by_hand():
    problem = hand()
    assert engine_score(problem, None) == (-(1 + 5 + 0 + 1), -(2 + 1 + 1))
    assert score(cl.constraint_costs(problem)) == (-7, -4.0)
    calculated = cl.calculate_score(problem)
    assert (calculated.hard, calculated.soft) == (-7, -4.0)


def test_stacking_on_top_counts_once_the_item_is_under_another():
    """In a space that is 8 high, b goes on top of a, which carries 5 of its 7."""
    problem = hand()
    containers = problem.containers
    containers[0].height = 8
    containers[0].length = 4
    problem.containers = containers
    raw = cl.constraint_costs(problem)
    assert raw["stacking_load_cost"]["costs"] == [2, 0]
    assert raw["unloading_order_cost"]["costs"] == [1, 0]  # b, a later stop, stands on a


def test_default_solve_equals_explicit_default_constraints():
    plain = solved()
    explicit = solved(get_constraints())
    assert (plain.score.hard, plain.score.soft) == (explicit.score.hard, explicit.score.soft)
    assert plain.loading() == explicit.loading()


def test_picking_a_subset_drops_the_other_terms():
    assert engine_score(hand(), [unplaced_item_cost]) == (-1, 0.0)
    assert engine_score(hand(), only(get_constraints(), "used_container_cost", "group_split_cost")) == (0, -4.0)
    assert engine_score(hand(), without(get_constraints(), "container_weight_cost")) == (-2, -4.0)


def test_reweighting_one_constraint_scales_only_that_term():
    picks = replace_default(get_constraints(), "used_container_cost", UsedContainerCost(weight=10))
    assert engine_score(hand(), picks) == (-7, -(30 + 1))
    picks = replace_default(get_constraints(), "container_weight_cost", ContainerWeightCost(weight=3))
    assert engine_score(hand(), picks) == (-(1 + 15 + 1), -4.0)


def test_the_opt_in_costs_score_once_they_have_a_weight():
    picks = replace_default(get_constraints(), "used_length_cost", UsedLengthCost(weight=0.5))
    assert engine_score(hand(), picks) == (-7, -(4 + 6.0))
    picks = replace_default(get_constraints(), "load_balance_cost", LoadBalanceCost(weight=0.01))
    assert engine_score(hand(), picks) == (-7, pytest.approx(-(4 + 0.01 * sum(BALANCE))))


def test_level_can_be_flipped():
    demoted = [
        UnplacedItemCost(level="soft"),
        ContainerWeightCost(level=Level.SOFT),
        UnloadingOrderCost(level="soft"),
        used_container_cost,
    ]
    assert engine_score(hand(), demoted) == (0, -(1 + 5 + 1 + 3))
    assert engine_score(hand(), [GroupSplitCost(level="hard"), UsedContainerCost(level=Level.HARD)]) == (-4, 0.0)
    with pytest.raises(ValueError, match="no medium level"):
        engine_score(hand(), [UsedContainerCost(level="medium")])


def test_exponent_shapes_per_container_and_the_one_total():
    assert engine_score(hand(), [ContainerWeightCost(exponent=2.0)]) == (-25, 0.0)
    assert engine_score(hand(), [UsedLengthCost(weight=1, exponent=2.0)]) == (0, -(64 + 16.0))  # not 12 ** 2
    assert engine_score(hand(), [UsedContainerCost(exponent=2.0)]) == (0, -(4 + 1.0))
    assert engine_score(hand(), [GroupSplitCost(weight=3, exponent=2.0)]) == (0, -3.0)


def test_offset_and_clip():
    # A threshold: only the length beyond 5 counts, so c1's 4 costs nothing.
    assert engine_score(hand(), [UsedLengthCost(weight=1, offset=5)]) == (0, -3.0)
    # The literal formula: every container is shifted, c1 below the offset lowers the cost.
    assert engine_score(hand(), [UsedLengthCost(weight=1, offset=5, clip=False)]) == (0, -(3 - 1.0))
    assert engine_score(hand(), [ContainerWeightCost(offset=2)]) == (-3, 0.0)
    with pytest.raises(ValueError, match="clip=False with a non-zero offset cannot be hard"):
        engine_score(hand(), [ContainerWeightCost(offset=2, clip=False)])


def test_naming_a_constraint_twice_scores_both_picks():
    picks = [UsedContainerCost(), UsedContainerCost(weight=10), GroupSplitCost(), GroupSplitCost(weight=5)]
    problem = hand()
    assert engine_score(problem, picks) == (0, -(11 * 3 + 6 * 1))
    raw = cl.constraint_costs(problem, picks)
    assert list(raw) == ["used_container_cost", "used_container_cost#2", "group_split_cost", "group_split_cost#2"]
    assert raw["used_container_cost#2"]["weight"] == 10 and raw["used_container_cost#2"]["costs"] == [2, 1]
    assert score(raw, picks) == (0, -39.0)
    assert get_constraints([UsedContainerCost(), UsedContainerCost()]) == [UsedContainerCost(), UsedContainerCost()]


def test_score_equals_the_engine_for_the_default_assembly():
    problem = solved()
    engine = cl.calculate_score(problem)
    assert score(cl.constraint_costs(problem)) == (engine.hard, engine.soft)
    assert score(cl.constraint_costs(problem), get_constraints()) == (engine.hard, engine.soft)


def test_score_is_additive_over_the_picked_constraints():
    problem = hand()
    raw = cl.constraint_costs(problem)
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_python_score_equals_the_engine_for_random_specs():
    """For random loadings and random specs (levels, exponents and offsets included): the
    pure-Python score of the raw costs == the engine's score under the same picks."""
    rng = random.Random(3)
    for _ in range(60):
        problem = cl.generate_demo_data(item_count=18, container_count=3, random_seed=rng.randrange(100))
        lists = [[] for _ in problem.containers]
        for item in range(len(problem.items)):
            rng.choice(lists).append(item)
        problem.set_loading(lists)
        picked = rng.sample(CLASSES, rng.randrange(1, len(CLASSES) + 1))
        specs = [
            spec(
                weight=rng.choice([None, 0, 0.5, 1, 3]),
                level=rng.choice([None, "hard", "soft"]),
                exponent=rng.choice([1.0, 0.5, 2.0]),
                offset=rng.choice([0.0, 0.0, 1.0, -1.0]),
            )
            for spec in picked
        ]
        python = score(cl.constraint_costs(problem, specs), specs)
        hard, soft = engine_score(problem, specs)
        assert python.hard == hard
        assert python.soft == pytest.approx(soft)


def test_a_solve_under_picks_reports_the_picks_score():
    picks = replace_default(get_constraints(), "used_length_cost", UsedLengthCost(weight=0.001))
    problem = solved(picks)
    python = score(cl.constraint_costs(problem, picks), picks)
    assert python.hard == problem.score.hard
    assert python.soft == pytest.approx(problem.score.soft)
    # The model path reports the same score: the selection's, not the one of the defaults.
    model = cl.models.ContainerLoadingProblemModel.from_core(cl.generate_demo_data(item_count=20, container_count=3))
    config = cl.SolverConfig(stop={"step_limit": 800}, run={"random_seed": 1})
    by_model = cl.solve(model, config=config, constraints=picks)
    assert (by_model.score_hard, by_model.score_soft) == (problem.score.hard, pytest.approx(problem.score.soft))
    assert by_model.score_soft < -by_model.containers_used  # the length is in it
