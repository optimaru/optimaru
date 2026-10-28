"""데모 solve: Java DemoDataGenerator 재현 인스턴스가 feasible 에 도달하고,
고정(pinned) 배정이 유지되는지 검증."""

import tournament_scheduling as ts
from tournament_scheduling.models import TournamentScheduleModel, demo_data


def test_demo_solve_reaches_feasible():
    model = demo_data()
    result = ts.solve(model, time_limit_seconds=5.0, random_seed=42)
    assert isinstance(result, TournamentScheduleModel)
    assert result.score_hard == 0  # feasible
    assert all(a.team is not None for a in result.team_assignments)
    # 팀별 배정 수가 유효한 분배인지 (합 == 72, 하루 1경기 제한 준수는 hard 0).
    loads = [0] * len(result.teams)
    for a in result.team_assignments:
        loads[a.team] += 1
    assert sum(loads) == 72
    # medium 최적 분배(11,11,10,10,10,10,10)의 불공정도 이상 좋을 수 없다.
    assert result.score_soft <= 0
    assert result.score_soft >= -(ts.unfairness([11, 11, 10, 10, 10, 10, 10]) * ts.MEDIUM_SCALE + 100)


def test_solve_core_in_place():
    core = demo_data().to_core()
    result = ts.solve(core, time_limit_seconds=2.0, random_seed=1)
    assert result is core
    assert core.score.is_feasible()
    assert all(t >= 0 for t in core.assignment())


def test_pinned_assignment_kept():
    model = demo_data()
    model.team_assignments[0].team = 6  # Maciej 를 Day 0 슬롯 0 에 고정
    model.team_assignments[0].pinned = True
    result = ts.solve(model, time_limit_seconds=2.0, random_seed=7)
    assert result.team_assignments[0].team == 6
    assert result.team_assignments[0].pinned is True
    assert result.score_hard == 0


def test_stop_at_first_feasible_and_callback():
    events = []
    result = ts.solve(
        demo_data(),
        time_limit_seconds=5.0,
        random_seed=3,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: events.append((step, score.hard)),
    )
    assert result.score_hard == 0
    assert events, "on_new_best 콜백이 한 번도 불리지 않았다"


def test_parallel_solver_count_workers():
    """parallelSolverCount>1 은 엔진 내장 multiStart 가 아니라 도메인 워커
    스레드로 팬아웃한다 (TeamCountMove 가 워커별 계산기에 바인딩되어야 하므로).
    워커별 독립 solve 가 정상 종료하고 best 가 채택되는지 확인한다."""
    for workers in (2, 4):
        core = demo_data().to_core()
        ts.solve(core, time_limit_seconds=2.0, random_seed=42, parallel_solver_count=workers)
        assert core.score.is_feasible()
        assert all(t >= 0 for t in core.assignment())
