"""데모 인스턴스 solve 테스트: feasible(hard 0) 도달과 결과 일관성."""

import project_job as pj
from project_job.demo import generate_demo_data
from project_job.models import ProjectJobScheduleModel


def test_demo_solve_reaches_feasible():
    model = generate_demo_data()
    result = pj.solve(model, time_limit_seconds=5.0, random_seed=42)

    assert isinstance(result, ProjectJobScheduleModel)
    assert result.score_hard == 0
    # 모든 할당이 초기화되어 있고, SOURCE/SINK 고정값은 유지된다.
    for job, allocation in zip(result.jobs, result.allocations):
        assert allocation.execution_mode_id is not None
        assert allocation.delay is not None
        if job.job_type != "STANDARD":
            assert allocation.execution_mode_id == job.execution_modes[0].id
            assert allocation.delay == 0
    # 결과 점수는 재채점과 일치해야 한다.
    replan = result.to_core()
    score = pj.calculate_score(replan)
    assert (score.hard, score.medium, score.soft) == (
        result.score_hard,
        result.score_medium,
        result.score_soft,
    )


def test_solve_core_schedule_in_place():
    schedule = generate_demo_data().to_core()
    returned = pj.solve(schedule, time_limit_seconds=3.0, random_seed=7, stop_at_first_feasible=True)
    assert returned is schedule
    assert schedule.score.is_feasible()
    value_count = schedule.value_count()
    assert all(0 <= value < value_count for value in schedule.assignment())
    # 파생 날짜가 채워져 있고 선행 관계를 지킨다.
    allocations = schedule.allocations
    for allocation in allocations:
        assert allocation.end_date >= allocation.start_date >= 0
        for predecessor in allocation.predecessor_allocations:
            assert allocations[predecessor].end_date <= allocation.start_date


def test_on_new_best_callback_fires():
    schedule = generate_demo_data().to_core()
    events: list[tuple[int, int]] = []
    pj.solve(
        schedule,
        time_limit_seconds=1.0,
        random_seed=1,
        on_new_best=lambda step, score: events.append((step, score.hard)),
    )
    assert events
    assert events[-1][1] == schedule.score.hard
