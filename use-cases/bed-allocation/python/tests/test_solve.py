"""데모 인스턴스 solve 테스트: feasible(hard 0) 도달과 결과 일관성."""

import bed_allocation as ba
from bed_allocation.demo import generate_demo_data
from bed_allocation.models import BedPlanModel


def test_demo_solve_reaches_feasible():
    model = generate_demo_data()
    result = ba.solve(model, time_limit_seconds=5.0, random_seed=42)

    assert isinstance(result, BedPlanModel)
    assert result.score_hard == 0
    # 결과 점수는 재채점과 일치해야 한다.
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
    # 모든 stay 가 초기화되어 있어야 한다 (실제 병상 또는 no-bed 센티널).
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
