"""데모 인스턴스 solve 테스트: feasible(hard 0, 전 태스크 배정) 도달과 결과
일관성."""

import task_assigning as ta
from task_assigning.demo import generate_demo_data
from task_assigning.models import TaskAssigningSolutionModel


def test_demo_solve_reaches_feasible():
    model = generate_demo_data()
    result = ta.solve(model, time_limit_seconds=5.0, random_seed=42)

    assert isinstance(result, TaskAssigningSolutionModel)
    assert result.score_hard == 0
    assert result.score_soft0 == 0  # 전 태스크 배정
    # 모든 task 가 정확히 한 직원에게 배정되어 있다.
    assigned = [task_id for employee in result.employees for task_id in employee.tasks]
    assert sorted(assigned) == sorted(t.id for t in result.tasks)
    # 결과 점수는 재채점과 일치해야 한다.
    resolution = result.to_core()
    score = ta.calculate_score(resolution)
    assert (score.hard, score.soft0, score.soft1, score.soft2) == (
        result.score_hard,
        result.score_soft0,
        result.score_soft1,
        result.score_soft2,
    )


def test_solve_core_solution_in_place():
    solution = generate_demo_data().to_core()
    returned = ta.solve(solution, time_limit_seconds=2.0, random_seed=7, stop_at_first_feasible=True)
    assert returned is solution
    assert solution.score.is_feasible()
    assert solution.score.soft0 == 0
    # start_time shadow 가 채워져 있어야 한다 (첫 task 는 min_start_time).
    for task in solution.tasks:
        assert task.employee >= 0
        assert task.start_time >= 0


def test_on_new_best_callback_fires():
    solution = generate_demo_data().to_core()
    events: list[tuple[int, int]] = []
    ta.solve(
        solution,
        time_limit_seconds=1.0,
        random_seed=1,
        on_new_best=lambda step, score: events.append((step, score.hard)),
    )
    assert events
    assert events[-1][1] == solution.score.hard
