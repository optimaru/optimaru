"""max_cut.solve / Solver / SolverConfig — the engine surface shared with the other C++ packages."""

import json
from dataclasses import replace
from pathlib import Path

import pytest

import max_cut
from max_cut.constraints import get_constraints, partition_size_bounds_cost
from max_cut.models import MaxCutProblemModel

FIXTURES = Path(__file__).parent / "fixtures"


def demo() -> MaxCutProblemModel:
    return MaxCutProblemModel.model_validate(json.loads((FIXTURES / "web_demo.json").read_text()))


def steps(count: int, seed: int = 1) -> max_cut.SolverConfig:
    config = max_cut.SolverConfig()
    config.step_count_limit = count
    config.random_seed = seed
    return config


def test_solver_config_defaults():
    config = max_cut.SolverConfig()
    assert config.spent_limit_seconds == 30.0
    assert (config.step_count_limit, config.unimproved_step_count_limit) == (0, 0)
    assert (config.random_seed, config.parallel_solver_count, config.late_acceptance_size) == (0, 1, 0)
    assert (config.change_move_weight, config.swap_move_weight) == (0.8, 0.2)
    assert config.ruin_recreate is True
    assert config.ruin_recreate_as_move_weight == 0.0
    assert config.descent_weight == 0.0
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
    config = max_cut.SolverConfig()
    with pytest.raises(ValueError):
        setattr(config, attribute, value)


def test_the_sa_knobs_are_gone():
    """0.2.0: the engine knobs replace the 0.1.x simulated-annealing ones."""
    config = max_cut.SolverConfig()
    for old in ("max_iterations", "stagnation_limit", "temperature", "cooling_rate"):
        assert not hasattr(config, old)
        with pytest.raises(AttributeError):
            setattr(config, old, 1)
    with pytest.raises(TypeError):
        max_cut.SolverConfig(max_iterations=0)
    for gone in ("anneal", "configure", "DEFAULTS", "score"):
        assert not hasattr(max_cut, gone)


def test_solve_on_the_engine_object_and_on_the_model_agree():
    core = demo().to_core()
    model = demo()
    by_core = max_cut.solve(core, config=steps(3000, seed=7))
    by_model = max_cut.solve(model, config=steps(3000, seed=7))
    assert isinstance(by_core, max_cut.Score)
    assert (by_core.hard, by_core.soft) == (by_model.hard, by_model.soft)
    assert model.partition == dict(zip(model.vertices, core.assignment(), strict=True))
    assert model.cut_weight == core.cut_weight()


def test_the_web_demo_solves_to_a_feasible_partition():
    """The sample has min = max = 4: the exact-size swap-only path."""
    model = demo()
    result = max_cut.solve(model, time_limit_seconds=1.0, random_seed=1)
    assert result.is_feasible()
    sides = model.partition
    assert sum(sides.values()) == 4
    assert sides["A"] == 0
    assert sides["B"] == sides["E"]
    assert sides["C"] != sides["F"]
    assert model.score_soft == model.cut_weight


def test_a_step_limit_reproduces_the_same_solve():
    first = demo().to_core()
    second = demo().to_core()
    max_cut.solve(first, config=steps(2000, seed=11))
    max_cut.solve(second, config=steps(2000, seed=11))
    assert first.assignment() == second.assignment()
    assert (first.score.hard, first.score.soft) == (second.score.hard, second.score.soft)


def test_keyword_arguments_override_the_config():
    config = steps(100, seed=5)
    calls = []
    problem = demo()
    problem.min_partition_size = problem.max_partition_size = None  # flips allowed: something improves
    max_cut.solve(
        problem,
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
    config = max_cut.SolverConfig()
    before = config.best_score_limit
    max_cut.solve(demo(), time_limit_seconds=5.0, stop_at_first_feasible=True, config=config)
    assert config.best_score_limit == before  # the score limit went to a copy


def test_multi_start_returns_a_consistent_best():
    core = demo().to_core()
    max_cut.solve(core, time_limit_seconds=0.3, random_seed=1, parallel_solver_count=4)
    fresh = demo().to_core()
    fresh.set_assignment(core.assignment())
    rescored = max_cut.calculate_score(fresh)
    assert (core.score.hard, core.score.soft) == (rescored.hard, rescored.soft)


@pytest.mark.parametrize("parallel_solver_count", [1, 2])
def test_a_raising_on_new_best_raises_from_solve(parallel_solver_count):
    """The callback's exception reaches the caller, also from a parallel worker thread (no process abort)."""

    def fail(step, score):
        raise RuntimeError("on_new_best failed")

    problem = demo()
    problem.min_partition_size = problem.max_partition_size = None  # flips allowed: something improves
    with pytest.raises(RuntimeError, match="on_new_best failed"):
        max_cut.solve(problem, time_limit_seconds=5.0, parallel_solver_count=parallel_solver_count, on_new_best=fail)


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path used to call std::terminate."""
    import time

    from max_cut import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            max_cut.solve(demo(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_full_assert_solves_without_corruption():
    for exact in (True, False):
        config = steps(3000, seed=3)
        config.full_assert = True
        config.ruin_recreate_unimproved_step_trigger = 100
        config.ruin_recreate_as_move_weight = 0.05
        model = demo()
        if not exact:
            model.min_partition_size = model.max_partition_size = None
        core = model.to_core()
        max_cut.Solver(config).solve(core)
        plugin = model.to_core()
        max_cut.Solver(config).solve_with_constraints(plugin, get_constraints())
        assert core.score.is_feasible() and plugin.score.is_feasible()


def test_the_constraints_path_scores_with_the_picks():
    core = demo().to_core()
    max_cut.Solver(steps(2000)).solve_with_constraints(core, get_constraints())
    default = max_cut.calculate_score(core)
    assert (core.score.hard, core.score.soft) == (default.hard, default.soft)


def test_the_server_flow_round_trips_with_a_reweighted_selection():
    """engine._solve_metaheuristics: to_core -> solve(core, constraints=[replace(spec, ...)]) -> from_core."""
    core = demo().to_core()
    selection = [*get_constraints()[:3], replace(partition_size_bounds_cost, weight=2.0), *get_constraints()[4:]]
    result = max_cut.solve(core, time_limit_seconds=0.3, random_seed=42, parallel_solver_count=1, constraints=selection)
    back = MaxCutProblemModel.from_core(core)
    assert (back.score_hard, back.score_soft) == (result.hard, result.soft)
    assert back.cut_weight == core.cut_weight() and set(back.partition) == set(back.vertices)
    assert (result.hard, result.soft) == tuple(max_cut.calculate_score(core, selection))


def test_zero_budget_returns_the_start_partition():
    """No warm start: vertex i on side i % 2, a fixed vertex on its side (0.1.x rule)."""
    model = demo()
    max_cut.solve(model, time_limit_seconds=0)
    assert model.partition == {vertex: index % 2 for index, vertex in enumerate(model.vertices)}  # A fixed to 0
    assert model.cut_weight is not None and model.warm_start is False


def test_calculate_score_fills_the_model():
    model = demo()
    model.partition = {"A": 0, "B": 1, "C": 1, "D": 0, "E": 1, "F": 0, "G": 1, "H": 0}
    result = max_cut.calculate_score(model)
    assert (result.hard, result.soft) == (0, 63.0)
    assert (model.cut_weight, model.score_hard, model.score_soft) == (63, 0, 63.0)
    picked = max_cut.calculate_score(model, get_constraints()[-1:])
    assert picked == (0, 63.0) and isinstance(picked, tuple)


def test_an_empty_problem_solves():
    core = max_cut.MaxCutProblem([])
    score = max_cut.solve(core, time_limit_seconds=1)
    assert (score.hard, score.soft) == (0, 0.0)
    model = MaxCutProblemModel(vertices=[], edges=[])
    max_cut.solve(model, time_limit_seconds=1)
    assert (model.partition, model.cut_weight, model.score_hard, model.score_soft) == ({}, 0, 0, 0.0)


def test_inputs_of_the_wrong_type_are_refused():
    with pytest.raises(TypeError, match="MaxCutProblemModel or a MaxCutProblem"):
        max_cut.solve({"vertices": [], "edges": []})
    with pytest.raises(TypeError):
        max_cut.constraint_costs([])


def test_stop_at_first_feasible_has_no_soft_floor(monkeypatch):
    """stop_at_first_feasible hands the engine best_score_limit (0, -inf): only the hard level
    is compared, so no soft score, however low, misses the stop. The first 0.2.0 limit,
    (0, -1e18), was never reached by a soft score below -1e18. A soft score that low cannot
    be built here (its default soft terms are a reward (cut_weight_cost) and an opt-in term at weight 0), so the limit itself is checked; the stop is exercised end to end
    in the other packages' tests."""
    import max_cut as package

    seen = []

    class Captured(Exception):
        pass

    class Capture:
        def __init__(self, cfg):
            seen.append(cfg.best_score_limit)
            raise Captured

    monkeypatch.setattr(package, "Solver", Capture)
    with pytest.raises(Captured):
        package.solve(demo(), stop_at_first_feasible=True)
    assert seen == [(0, float("-inf"))]
