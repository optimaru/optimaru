import sys

import pytest

from container_loading_cpsat import benchmark, evaluator, instances, known_optimum
from container_loading_cpsat import model as cpsat
from container_loading_cpsat.evaluator import constraints as pricing
from optimaru import container_loading


def test_the_comparison_uses_the_engine_in_this_checkout():
    assert sys.modules["optimaru.container_loading"] is container_loading
    assert benchmark.PACKAGE == evaluator.__name__


def test_every_instance_builds_and_knows_its_optimum():
    ladder = benchmark.instances()
    assert list(ladder)[:2] == ["tiny", "tiny-2"] and "xlarge" in ladder
    for name, build in ladder.items():
        problem = build()
        assert problem is not None
        assert "optimum" in benchmark.describe(problem), name


def test_a_cut_container_is_a_perfect_packing():
    """The boxes of every cut add up to the containers they were cut from, and the instance is
    the same for the same seed."""
    model = instances.perfect_packing(3, 6, seed=5)
    capacity = 12 * 8 * 8
    assert sum(item.length * item.width * item.height for item in model.items) == 3 * capacity
    assert len(model.items) == 18 and len(model.containers) == 5
    assert instances.optimum(model) == 3
    assert model == instances.perfect_packing(3, 6, seed=5)
    assert model != instances.perfect_packing(3, 6, seed=6)
    slack = instances.perfect_packing(10, 20, seed=5, slack=0.08)
    assert instances.optimum(slack) == 10
    assert 0.92 * 10 * capacity <= sum(i.length * i.width * i.height for i in slack.items) < 10 * capacity


def test_cpsat_proves_the_optimum_of_a_small_instance_and_the_engine_matches_it():
    model = instances.perfect_packing(2, 4, seed=1)
    answer = cpsat.solve(model, seconds=20, seed=1, workers=2)
    assert answer.proven and answer.status == "OPTIMAL"
    assert (answer.solved.score_hard, answer.solved.score_soft) == (0, -2.0)
    assert evaluator.loading_faults(answer.solved) == []
    engine = container_loading.solve(model, time_limit_seconds=2, random_seed=1)
    assert (engine.score_hard, engine.score_soft) == (0, -2.0)


def test_the_evaluator_scores_the_engines_answer_as_the_engine_does():
    """Positions in, costs out: the same raw costs the engine reports off its decoder."""
    problem = container_loading.generate_demo_data(item_count=24, container_count=3)
    container_loading.solve(problem, time_limit_seconds=0.5, random_seed=1)
    expected = container_loading.constraint_costs(problem)
    assert evaluator.constraint_costs(problem) == expected
    score = pricing.score(evaluator.constraint_costs(problem))
    assert (score.hard, score.soft) == (problem.score.hard, problem.score.soft)


def test_the_evaluator_refuses_positions_that_are_no_loading():
    solved = container_loading.solve(instances.perfect_packing(1, 4, seed=2), time_limit_seconds=0.2)
    moved = solved.model_copy(deep=True)
    placed = next(item for item in moved.items if item.sequence == 1)
    first = next(item for item in moved.items if item.sequence == 0)
    placed.x, placed.y, placed.z = first.x, first.y, first.z
    with pytest.raises(ValueError, match="not a valid loading"):
        evaluator.constraint_costs(moved)


def test_the_model_covers_weight_limits_costs_and_groups():
    """Two boxes that fit one small carton together, but weigh more than it carries: the model
    opens the second carton instead of the large, dear one, and counts the split group."""
    from optimaru.container_loading.models import ContainerLoadingProblemModel

    model = ContainerLoadingProblemModel(
        containers=[
            {"id": "small-1", "length": 4, "width": 2, "height": 2, "max_weight": 5, "cost": 1.0},
            {"id": "small-2", "length": 4, "width": 2, "height": 2, "max_weight": 5, "cost": 1.0},
            {"id": "large", "length": 6, "width": 4, "height": 4, "cost": 3.5},
        ],
        items=[
            {"id": "a", "length": 2, "width": 2, "height": 2, "weight": 4, "group": "order"},
            {"id": "b", "length": 2, "width": 2, "height": 2, "weight": 4, "group": "order"},
        ],
    )
    answer = cpsat.solve(model, seconds=10, workers=2)
    assert answer.proven
    # Two small cartons and the split (1 + 1 + 1) beat the large one (3.5).
    assert (answer.solved.score_hard, answer.solved.score_soft) == (0, -3.0)
    assert sorted(item.container for item in answer.solved.items) == ["small-1", "small-2"]
    engine = container_loading.solve(model, time_limit_seconds=1)
    assert (engine.score_hard, engine.score_soft) == (0, -3.0)


def test_the_model_refuses_what_it_does_not_cover():
    model = instances.perfect_packing(1, 3, seed=1)
    model.items[0].unload_order = 1
    with pytest.raises(ValueError, match="neither max_load_on_top nor unload_order"):
        cpsat.solve(model, seconds=1)
    large = instances.perfect_packing(6, 15, seed=1)
    with pytest.raises(RuntimeError, match=r"status=TOO_LARGE"):
        cpsat.solve(large, seconds=1)
    priced = instances.perfect_packing(1, 3, seed=1)
    priced.containers[0].cost = 1.00001
    with pytest.raises(ValueError, match="whole number of 1/1000"):
        cpsat.solve(priced, seconds=1)


def test_the_known_optimum_table_runs(capsys):
    assert known_optimum.main(["--seconds", "0.2", "--families", "1"]) == 0
    lines = capsys.readouterr().out.strip().splitlines()
    assert lines[0].startswith("| cut containers x boxes") and len(lines) == 3
    assert lines[0].split(" | ")[4:] == ["volume needs", "construction", "solve |"]
    assert lines[2].startswith("| 4 x 15 | 0% | fixed | 60 | 4 |")
