"""Demo instance solve test: reaching feasible (hard 0) and result consistency."""

import bed_allocation as ba
from bed_allocation.demo import generate_demo_data
from bed_allocation.models import BedPlanModel


def test_demo_solve_reaches_feasible():
    model = generate_demo_data()
    result = ba.solve(model, time_limit_seconds=5.0, random_seed=42)

    assert isinstance(result, BedPlanModel)
    assert result.score_hard == 0
    # The result score must match a rescoring.
    replan = result.to_core()
    score = ba.calculate_score(replan)
    assert (score.hard, score.medium, score.soft) == (
        result.score_hard,
        result.score_medium,
        result.score_soft,
    )


def test_solve_core_plan_in_place():
    plan = generate_demo_data().to_core()
    returned = ba.solve(plan, time_limit_seconds=2.0, random_seed=7, stop_at_first_feasible=True)
    assert returned is plan
    assert plan.score.is_feasible()
    # Every stay must be initialized (a real bed or the no-bed sentinel).
    no_bed = plan.no_bed_value()
    assert all(0 <= value <= no_bed for value in plan.assignment())


def test_on_new_best_callback_fires():
    plan = generate_demo_data().to_core()
    events: list[tuple[int, int]] = []
    ba.solve(
        plan,
        time_limit_seconds=1.0,
        random_seed=1,
        on_new_best=lambda step, score: events.append((step, score.hard)),
    )
    assert events
    assert events[-1][1] == plan.score.hard


def test_solve_leaves_the_callers_config_alone():
    """The keyword overrides go to a copy of config= (they leaked into later solves)."""
    import copy

    config = ba.SolverConfig()
    config.spent_limit_seconds = 5.0
    config.random_seed = 3
    before = (config.spent_limit_seconds, config.random_seed, config.best_score_limit)
    ba.solve(
        generate_demo_data().to_core(),
        time_limit_seconds=0,
        random_seed=77,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: None,
        config=config,
    )
    assert (config.spent_limit_seconds, config.random_seed, config.best_score_limit) == before
    assert config.on_new_best is None
    clone = copy.copy(config)
    clone.random_seed = 9
    assert config.random_seed == 3 and copy.deepcopy(config).random_seed == 3


def test_time_limits_must_be_finite_and_non_negative():
    """The same rule in every package (some accepted -1, all accepted NaN and inf)."""
    import pytest

    config = ba.SolverConfig()
    for name in ("spent_limit_seconds", "unimproved_spent_limit_seconds"):
        for bad in (-1.0, float("nan"), float("inf")):
            with pytest.raises(ValueError, match=f"{name} must be finite and >= 0"):
                setattr(config, name, bad)
        setattr(config, name, 0.0)  # a zero budget is valid
    with pytest.raises(ValueError, match="spent_limit_seconds must be finite and >= 0"):
        ba.solve(generate_demo_data().to_core(), time_limit_seconds=-1)


def test_time_limits_are_at_most_1e9_seconds():
    """A huge finite limit would overflow the engine's int64 clock arithmetic and read back as a
    negative budget, stopping the solve after construction; it is refused."""
    import sys

    import pytest

    config = ba.SolverConfig()
    for name in ("spent_limit_seconds", "unimproved_spent_limit_seconds"):
        for huge in (1e9 + 1, 1e10, 1e16, float(sys.maxsize), 1e300):
            with pytest.raises(ValueError, match=f"{name} must be at most 1e9 seconds"):
                setattr(config, name, huge)
        setattr(config, name, 1e9)  # the limit itself is valid and reads back exactly
        assert getattr(config, name) == 1e9
    with pytest.raises(ValueError, match="spent_limit_seconds must be at most 1e9 seconds"):
        ba.solve(generate_demo_data().to_core(), time_limit_seconds=sys.maxsize)

    # The largest limit is a real budget: a step-limited solve runs the same steps as under a
    # 30 s limit instead of stopping after construction.
    def solved(limit: float) -> list[int]:
        config = ba.SolverConfig()
        config.spent_limit_seconds = limit
        config.step_count_limit = 2000
        config.random_seed = 3
        plan = generate_demo_data().to_core()
        ba.Solver(config).solve(plan)
        return plan.assignment()

    assert solved(1e9) == solved(30.0) != solved(0.0)


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    import pytest

    def boom(_step, _score):
        raise RuntimeError("boom")

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        ba.solve(
            generate_demo_data().to_core(),
            time_limit_seconds=30.0,
            parallel_solver_count=4,
            on_new_best=boom,
        )
    assert time.monotonic() - started < 10.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path would call std::terminate."""
    import time

    import pytest

    from bed_allocation import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            ba.solve(generate_demo_data().to_core(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_stop_at_first_feasible_has_no_soft_floor():
    """-(2**63) (INT64_MIN) on the soft-side levels means "no floor": the engine compares
    hard only, so no soft score, however low, misses the stop (toEngineScoreLimit,
    tested in the C++ suite)."""
    cfg = ba._build_config(
        time_limit_seconds=None,
        random_seed=None,
        parallel_solver_count=None,
        stop_at_first_feasible=True,
        full_assert=False,
        on_new_best=None,
        config=None,
    )
    assert cfg.best_score_limit == (0, -(2**63), -(2**63))
