"""데모 인스턴스 solve: feasible(hard 0) 도달 + 배정/핀 보존 검증."""

import conference_scheduling as cs


def test_demo_data_shape():
    schedule = cs.demo_conference()
    assert schedule.talk_count() == 15
    assert schedule.timeslot_count() == 6
    assert schedule.room_count() == 5
    assert len(schedule.speakers) == 12
    # java.util.Random(0) 드로우 재현: S01 의 테마/콘텐츠 태그가 결정적이다.
    assert schedule.talks[0].theme_track_tags[0] in ("Optimization", "AI", "Cloud")
    codes = [t.code for t in schedule.talks]
    assert codes[0] == "S01" and codes[-1] == "S15"
    # S05 는 S02 를 선행 조건으로 가진다.
    assert schedule.talks[4].prerequisite_talks == [1]
    # S11 은 Recorded 방을 요구한다.
    assert schedule.talks[10].required_room_tags == ["Recorded"]


def test_demo_solve_feasible():
    schedule = cs.demo_conference()
    best = []
    cs.solve(
        schedule,
        time_limit_seconds=10.0,
        random_seed=42,
        on_new_best=lambda step, score: best.append((step, score.hard)),
    )
    assert schedule.score.is_feasible(), str(schedule.score)
    assert schedule.score.hard == 0
    assert all(v >= 0 for v in schedule.assignment())
    assert best, "on_new_best should fire at least once"
    # 최종 스코어는 full recompute 와 일치한다.
    recomputed = cs.calculate_score(schedule)
    assert recomputed.hard == schedule.score.hard
    assert recomputed.soft == schedule.score.soft
    # 타입 호환: Lab 토크는 Lab 슬롯/룸에만 있다 (synthetic hard 0 의 의미).
    b = cs.compute_breakdown(schedule)
    assert b.talk_type_compatibility == 0
    assert b.room_conflict == 0
    assert b.speaker_conflict == 0


def test_pinned_talk_survives_solve():
    schedule = cs.demo_conference()
    pinned_value = schedule.slot_room_of(0, 4)  # S01 (Lab) -> T1 x Room E
    talks = schedule.talks
    talks[0].slot_room = pinned_value
    talks[0].pinned = True
    schedule.talks = talks

    cs.solve(schedule, time_limit_seconds=2.0, random_seed=1, stop_at_first_feasible=True)
    assert schedule.talks[0].slot_room == pinned_value
    assert schedule.score.hard == 0


def test_solve_config_mirror():
    from conference_scheduling.models import SolveConfig

    cfg = SolveConfig(time_limit_seconds=1.0, random_seed=7, stop_at_first_feasible=True)
    schedule = cs.demo_conference()
    cs.solve(schedule, config=cfg)
    assert schedule.score.hard == 0


def test_parallel_solver_count_workers():
    """parallelSolverCount>1 은 엔진 내장 multiStart 가 아니라 도메인 레벨
    팬아웃으로 처리된다 (restartSegments 구간 예산을 도메인이 몰기 때문).
    워커별 solve 가 정상 종료하고 best 가 채택되는지 확인한다."""
    for workers in (2, 4):
        schedule = cs.demo_conference()
        cs.solve(
            schedule,
            time_limit_seconds=2.0,
            random_seed=42,
            parallel_solver_count=workers,
            stop_at_first_feasible=True,
        )
        assert schedule.score.hard == 0
        recomputed = cs.calculate_score(schedule)
        assert recomputed.hard == schedule.score.hard
        assert recomputed.soft == schedule.score.soft
