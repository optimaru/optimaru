"""solve(): the engine knobs, the samples and the callbacks."""

import json
import random
from pathlib import Path

import pytest

import bin_packing as bp
from bin_packing.constraints import (
    BinCapacityCost,
    ColorConflictCost,
    ItemConflictCost,
    UsedBinsCost,
    get_constraints,
)
from bin_packing.models import BinPackingProblemModel

WEB_DEMO = Path(__file__).parent / "fixtures" / "web_demo.json"


def web_demo() -> bp.BinPackingProblem:
    return BinPackingProblemModel.model_validate(json.loads(WEB_DEMO.read_text())).to_core()


def test_web_sample_reaches_the_eight_bin_lower_bound():
    """746 total weight on capacity 100: 8 bins, with every colour and conflict respected."""
    problem = web_demo()
    assert sum(item.weight for item in problem.items) == 746
    config = bp.SolverConfig()
    config.spent_limit_seconds = 10
    config.best_score_limit = (0, -8.0)
    config.random_seed = 7
    assert bp.solve(problem, config=config) == bp.Score(0, -8.0)
    assert problem.bins_used() == 8
    assert all(load > 0 for load in problem.bin_loads())  # compacted, no gaps


def test_web_sample_with_constraints_reaches_the_same_optimum():
    problem = web_demo()
    config = bp.SolverConfig()
    config.spent_limit_seconds = 10
    config.best_score_limit = (0, -8.0)
    config.random_seed = 7
    assert bp.solve(problem, config=config, constraints=get_constraints()) == bp.Score(0, -8.0)


def test_on_new_best_reports_step_and_score():
    seen = []
    problem = web_demo()
    bp.solve(problem, time_limit_seconds=1.0, random_seed=3, on_new_best=lambda step, score: seen.append((step, score)))
    assert seen
    assert all(isinstance(step, int) and isinstance(score, bp.Score) for step, score in seen)
    assert [score for _, score in seen] == sorted(score for _, score in seen)
    assert seen[-1][1] == problem.score


def test_stop_at_first_feasible_stops_early():
    problem = web_demo()
    bp.solve(problem, time_limit_seconds=30.0, random_seed=3, stop_at_first_feasible=True)
    assert problem.score.hard == 0


def test_step_count_limit_makes_the_search_deterministic():
    def run():
        problem = web_demo()
        config = bp.SolverConfig()
        config.step_count_limit = 3000
        config.random_seed = 9
        bp.solve(problem, config=config)
        return problem.assignment(), problem.score

    assert run() == run()


def test_constraints_path_scores_with_the_picks():
    problem = web_demo()
    picks = [*get_constraints()[:3], UsedBinsCost(weight=5)]
    config = bp.SolverConfig()
    config.step_count_limit = 2000
    config.random_seed = 2
    score = bp.solve(problem, config=config, constraints=picks)
    assert score == bp.calculate_score(problem, picks)
    assert score.soft == -5 * problem.bins_used()


def test_empty_problem_scores_zero_and_clears_the_seed():
    problem = bp.BinPackingProblem(10, [], warm_start=True)
    assert bp.solve(problem, time_limit_seconds=0.0) == bp.Score()
    assert problem.warm_start is False


@pytest.mark.parametrize(
    ("attribute", "value"),
    [
        ("parallel_solver_count", 0),
        ("late_acceptance_size", -1),
        ("spent_limit_seconds", -1.0),
        ("empty_bin_candidates", 0),
        ("repack_partner_bins", -1),
        ("ruin_recreate_rewards", [1.0]),
    ],
)
def test_solver_config_refuses_bad_values(attribute, value):
    with pytest.raises(ValueError):
        setattr(bp.SolverConfig(), attribute, value)


def test_the_simulated_annealing_knobs_are_gone():
    config = bp.SolverConfig()
    for old in ("max_iterations", "stagnation_limit", "temperature", "cooling_rate"):
        assert not hasattr(config, old)
    with pytest.raises(TypeError):
        bp.SolverConfig(max_iterations=0)


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    def boom(_step, _score):
        raise RuntimeError("boom")

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        bp.solve(web_demo(), time_limit_seconds=30.0, parallel_solver_count=4, on_new_best=boom)
    assert time.monotonic() - started < 10.0


def coloured_instance(seed: int = 20261031, count: int = 120) -> bp.BinPackingProblem:
    """The 0.2.0 review's coloured instance (SQ-1): weights 10..60 on capacity 100, about
    40% of the items in one of 4 colours, 10 conflict pairs. Seed 20261031 has a weight
    lower bound of 43 bins (4288 / 100); 0.1.5 used 44."""
    rng = random.Random(seed)
    for _ in range(60):  # the review recipe's burn-in, kept so the instance is the same
        rng.randint(1, 10)
    for j in range(1, 60):
        rng.sample(range(j), min(j, rng.choice([0, 1, 1, 2])))
    items = []
    for index in range(count):
        item = {"id": f"I{index:03d}", "weight": rng.randint(10, 60)}
        if rng.random() < 0.4:
            item["color"] = rng.choice(["red", "blue", "green", "yellow"])
        items.append(item)
    conflicts = []
    for _ in range(10):
        first, second = rng.sample(range(count), 2)
        conflicts.append({"first": items[first]["id"], "second": items[second]["id"]})
    data = {"bin_capacity": 100, "items": items, "conflicts": conflicts}
    return BinPackingProblemModel.model_validate(data).to_core()


def test_repack_moves_close_the_bins_colours_and_conflicts_keep_open():
    """EmptyBinMove alone (the 0.2.0 search before RepackMove) ends at 45-46 bins on this
    instance at 2-30 s, 0.1.5 at 44; with RepackMove the step-limited search gets there."""
    bins = {}
    for repack in (True, False):
        problem = coloured_instance()
        config = bp.SolverConfig()
        config.step_count_limit = 50_000
        config.random_seed = 1
        config.repack_moves = repack
        score = bp.solve(problem, config=config)
        assert score.hard == 0
        bins[repack] = problem.bins_used()
    assert bins[True] <= 44
    assert bins[True] < bins[False]


@pytest.mark.parametrize("shaped", [False, True])
def test_full_assert_solve_with_the_macro_moves(shaped):
    """FULL_ASSERT re-checks the incremental score after every move, RepackMove and
    EmptyBinMove included, on the default calculator and on a re-weighted assembly."""
    problem = coloured_instance(seed=7, count=40)
    picks = (
        [BinCapacityCost(weight=2), ColorConflictCost(exponent=2.0), ItemConflictCost(), UsedBinsCost(weight=3)]
        if shaped
        else None
    )
    config = bp.SolverConfig()
    config.full_assert = True
    config.step_count_limit = 3000
    config.random_seed = 3
    score = bp.solve(problem, config=config, constraints=picks)
    assert score == bp.calculate_score(problem, picks)
    assert all(item.bin >= 0 for item in problem.items)
