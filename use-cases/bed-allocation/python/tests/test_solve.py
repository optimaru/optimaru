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
