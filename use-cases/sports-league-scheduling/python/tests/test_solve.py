"""고수준 solve() 검증: 데모 인스턴스가 feasible(hard 0)에 도달한다."""

import pytest

import sports_league as sl
from sports_league.models import LeagueScheduleModel, SolveConfig


def test_demo_solve_feasible():
    schedule = sl.demo_schedule()
    result = sl.solve(
        schedule,
        time_limit_seconds=20.0,
        random_seed=42,
        stop_at_first_feasible=True,
    )
    assert result is schedule  # in place
    assert schedule.score.is_feasible()
    assert schedule.score.hard == 0
    assert all(m.round >= 0 for m in schedule.matches)

    # hard 1 재확인: 어떤 팀도 같은 라운드에 두 경기를 뛰지 않는다.
    seen: set[tuple[int, int]] = set()
    for m in schedule.matches:
        ri = schedule.rounds[m.round].index
        for team in (m.home_team, m.away_team):
            assert (team, ri) not in seen
            seen.add((team, ri))

    # 풀린 결과도 pydantic 미러로 왕복된다.
    model = LeagueScheduleModel.from_core(schedule)
    clone = LeagueScheduleModel.model_validate_json(model.model_dump_json())
    assert clone.to_core().assignment() == schedule.assignment()


def test_solve_accepts_config_objects():
    schedule = sl.demo_schedule()
    sl.solve(
        schedule,
        config=SolveConfig(time_limit_seconds=2.0, random_seed=1),
    )
    assert all(m.round >= 0 for m in schedule.matches)

    with pytest.raises(ValueError, match="config"):
        sl.solve(sl.demo_schedule(), config=object())


def test_on_new_best_callback():
    schedule = sl.demo_schedule()
    bests: list[tuple[int, float]] = []
    sl.solve(
        schedule,
        time_limit_seconds=2.0,
        random_seed=0,
        on_new_best=lambda step, score: bests.append((score.hard, score.soft)),
    )
    assert bests, "on_new_best 가 한 번도 호출되지 않았다"
    # best 점수는 단조 개선된다 (hard 우선 사전식).
    assert bests == sorted(bests)


def test_parallel_solver_count_workers():
    """parallelSolverCount>1 은 엔진 내장 multiStart 가 아니라 워커별 단일
    스타트로 팬아웃한다 (가이드 훅이 워커의 working solution/계산기에
    바인딩되므로). 각 워커가 정상 종료하고 best 가 채택되는지 확인한다."""
    for workers in (2, 4):
        schedule = sl.demo_schedule()
        sl.solve(
            schedule,
            time_limit_seconds=2.0,
            random_seed=42,
            parallel_solver_count=workers,
            stop_at_first_feasible=True,
        )
        assert schedule.score.hard == 0
        assert all(m.round >= 0 for m in schedule.matches)
