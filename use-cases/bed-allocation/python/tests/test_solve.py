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
