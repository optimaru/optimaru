"""knapsack.solve / Solver / SolverConfig — the engine surface shared with the other C++ packages."""

import json
import time
from pathlib import Path

import pytest

import knapsack
from knapsack.constraints import get_constraints
from knapsack.models import KnapsackProblemModel

FIXTURES = Path(__file__).parent / "fixtures"


def demo() -> KnapsackProblemModel:
    return KnapsackProblemModel.model_validate(json.loads((FIXTURES / "web_demo.json").read_text()))


def steps(count: int, seed: int = 1) -> knapsack.SolverConfig:
    config = knapsack.SolverConfig()
    config.step_count_limit = count
    config.random_seed = seed
    return config


def test_solver_config_defaults():
    config = knapsack.SolverConfig()
    assert config.spent_limit_seconds == 30.0
    assert (config.step_count_limit, config.unimproved_step_count_limit) == (0, 0)
    assert (config.random_seed, config.parallel_solver_count, config.late_acceptance_size) == (0, 1, 0)
    assert (config.change_move_weight, config.swap_move_weight) == (0.5, 0.5)
    assert config.ruin_recreate is True
    assert config.ruin_recreate_as_move_weight == 0.05
    assert (config.descent_weight, config.nearby_probability) == (0.0, 0.0)
    assert config.full_assert is False
    assert config.on_new_best is None


@pytest.mark.parametrize(
    "attribute, value",
    [
        ("spent_limit_seconds", -1.0),
        ("unimproved_spent_limit_seconds", -1.0),
        ("parallel_solver_count", 0),
        ("late_acceptance_size", -1),
        ("ruin_recreate_rewards", [1.0, 2.0]),
    ],
)
def test_solver_config_refuses_invalid_values(attribute, value):
    config = knapsack.SolverConfig()
    with pytest.raises(ValueError):
        setattr(config, attribute, value)


def test_the_sa_knobs_are_gone():
    """0.2.0: the engine knobs replace the 0.1.x simulated-annealing ones."""
    config = knapsack.SolverConfig()
    for old in ("max_iterations", "stagnation_limit", "temperature", "cooling_rate"):
        assert not hasattr(config, old)
        with pytest.raises(AttributeError):
            setattr(config, old, 1)
    with pytest.raises(TypeError):
        knapsack.SolverConfig(max_iterations=0)
    for gone in ("anneal", "configure", "DEFAULTS", "_greedy_selection"):
        assert not hasattr(knapsack, gone)


def test_solve_on_the_engine_object_and_on_the_model_agree():
    core = demo().to_core()
    model = demo()
    by_core = knapsack.solve(core, config=steps(3000, seed=7))
    by_model = knapsack.solve(model, config=steps(3000, seed=7))
    assert isinstance(by_core, knapsack.Score)
    assert (by_core.hard, by_core.soft) == (by_model.hard, by_model.soft)
    assert [item.selected for item in model.items] == [value == 1 for value in core.assignment()]
    assert (model.total_weight, model.total_value) == (core.total_weight(), core.total_value())


def test_the_web_demo_solves_to_a_feasible_selection():
    model = demo()
    result = knapsack.solve(model, time_limit_seconds=1.0, random_seed=1)
    assert result.is_feasible()
    assert model.total_weight <= model.capacity
    chosen = {item.id for item in model.items if item.selected}
    assert "first-aid" in chosen
    assert not {"tent", "drone"} <= chosen
    assert "drone" not in chosen or "battery" in chosen
    assert ("camera" in chosen) + 2 * ("drone" in chosen) <= 2  # fragile_slots capacity 2
    assert model.score_soft == model.total_value


def test_a_step_limit_reproduces_the_same_solve():
    first = demo().to_core()
    second = demo().to_core()
    knapsack.solve(first, config=steps(2000, seed=11))
    knapsack.solve(second, config=steps(2000, seed=11))
    assert first.assignment() == second.assignment()
    assert (first.score.hard, first.score.soft) == (second.score.hard, second.score.soft)


def test_keyword_arguments_override_the_config():
    config = steps(100, seed=5)
    calls = []
    knapsack.solve(
        demo(),
        time_limit_seconds=2.0,
        random_seed=9,
        parallel_solver_count=2,
        on_new_best=lambda step, score: calls.append((step, score.hard, score.soft)),
        config=config,
    )
    # The overrides applied (on_new_best fired) to a copy: the caller's config is unchanged.
    assert calls and all(isinstance(step, int) for step, _, _ in calls)
    assert (config.spent_limit_seconds, config.random_seed, config.parallel_solver_count) == (30.0, 5, 1)
    assert config.step_count_limit == 100 and config.on_new_best is None


def test_stop_at_first_feasible_leaves_the_callers_config_alone():
    config = knapsack.SolverConfig()
    before = config.best_score_limit
    knapsack.solve(demo(), time_limit_seconds=5.0, stop_at_first_feasible=True, config=config)
    assert config.best_score_limit == before  # the score limit went to a copy


def test_multi_start_returns_a_consistent_best():
    core = demo().to_core()
    knapsack.solve(core, time_limit_seconds=0.3, random_seed=1, parallel_solver_count=4)
    fresh = demo().to_core()
    fresh.set_assignment(core.assignment())
    rescored = knapsack.calculate_score(fresh)
    assert (core.score.hard, core.score.soft) == (rescored.hard, rescored.soft)


def test_full_assert_solves_without_corruption():
    config = steps(3000, seed=3)
    config.full_assert = True
    config.ruin_recreate_unimproved_step_trigger = 100
    core = demo().to_core()
    knapsack.Solver(config).solve(core)
    plugin = demo().to_core()
    knapsack.Solver(config).solve_with_constraints(plugin, get_constraints())
    assert core.score.is_feasible() and plugin.score.is_feasible()


def test_the_constraints_path_scores_with_the_picks():
    core = demo().to_core()
    knapsack.Solver(steps(2000)).solve_with_constraints(core, get_constraints())
    default = knapsack.calculate_score(core)
    assert (core.score.hard, core.score.soft) == (default.hard, default.soft)


def test_zero_budget_returns_the_greedy_construction():
    model = demo()
    knapsack.solve(model, time_limit_seconds=0)
    core = demo().to_core()
    greedy = knapsack._core.greedy_selection(core, [-1] * core.item_count())
    assert [item.selected for item in model.items] == greedy


def test_calculate_score_fills_the_model():
    model = demo()
    for item in model.items:
        item.selected = item.id in {"laptop", "first-aid"}
    result = knapsack.calculate_score(model)
    assert (result.hard, result.soft) == (0, 53.0)
    assert (model.total_weight, model.total_value, model.score_hard, model.score_soft) == (24, 53, 0, 53.0)
    picked = knapsack.calculate_score(model, get_constraints()[-1:])
    assert picked == (0, 53.0) and isinstance(picked, tuple)


def test_an_empty_problem_solves():
    core = knapsack.KnapsackProblem(5, [])
    score = knapsack.solve(core, time_limit_seconds=1)
    assert (score.hard, score.soft) == (0, 0.0)


def test_inputs_of_the_wrong_type_are_refused():
    with pytest.raises(TypeError, match="KnapsackProblemModel or a KnapsackProblem"):
        knapsack.solve({"capacity": 1, "items": []})
    with pytest.raises(TypeError):
        knapsack.constraint_costs([])


def _boom(_step, _score):
    raise RuntimeError("boom")


def test_a_raising_on_new_best_raises_from_a_multi_start_solve():
    """Two parallel starts: the callback's exception reaches Python instead of aborting the process,
    and the other start stops at once instead of running out its 10 s budget."""
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        knapsack.solve(
            demo(),
            time_limit_seconds=10.0,
            parallel_solver_count=2,
            on_new_best=_boom,
        )
    assert time.monotonic() - started < 8.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path used to call std::terminate."""
    from knapsack import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            knapsack.solve(demo(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_stop_at_first_feasible_stops_whatever_the_soft_total():
    """stop_at_first_feasible compares the hard level only (best_score_limit (0, -inf)).

    Every enabled soft penalty at weight 1e21 puts the soft score far below -1e18 at the
    first feasible solution: the solve still stops there instead of running its whole
    30 s. The first 0.2.0 limit, (0, -1e18), was never reached by such a score.
    """
    import dataclasses
    import time

    import knapsack as package
    from knapsack.constraints import get_constraints

    info = {row.name: row for row in package.constraint_info()}

    def heavy(spec):
        row = info[spec.name]
        soft_penalty = row.default_level == "soft" and row.default_enabled and not row.reward
        return dataclasses.replace(spec, weight=1e21) if soft_penalty else spec

    seen = []
    started = time.monotonic()
    package.solve(
        demo(),
        time_limit_seconds=30.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda _step, score: seen.append(score),
        constraints=[heavy(spec) for spec in get_constraints()],
    )
    assert time.monotonic() - started < 20.0
    assert seen[-1].hard == 0 and seen[-1].soft < -1e18
