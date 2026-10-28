"""데모 solve 검증: SMALL 데모가 feasible 에 도달하고 배정이 일관적인지."""

from itertools import pairwise

import maintenance_scheduling as mnt

# 고정 월요일 (2021-02-01) 로 Java 데모 인스턴스를 재현한다.
MONDAY = 18659


def test_demo_data_matches_java_shape():
    small = mnt.generate_demo_data("SMALL", MONDAY)
    assert small.crew_count() == 3
    assert len(small.jobs) == 14
    assert small.to_date - small.from_date == 8 * 7
    assert small.date_count() == 8 * 5  # weekday-only value range

    large = mnt.generate_demo_data("LARGE", MONDAY)
    assert large.crew_count() == 5
    assert len(large.jobs) == 48

    # 데모 작업의 min/max/ideal 윈도우는 일관적이어야 한다.
    for i, job in enumerate(small.jobs):
        assert 1 <= job.duration_in_days <= 10
        assert job.min_start_date is not None
        assert job.ideal_end_date is not None
        assert job.max_end_date is not None
        assert job.min_start_date < job.ideal_end_date <= job.max_end_date
        assert mnt.plus_business_days(job.min_start_date, job.duration_in_days) <= job.max_end_date, i


def test_demo_solve_reaches_feasible():
    schedule = mnt.generate_demo_data("SMALL", MONDAY)
    steps = []
    mnt.solve(
        schedule,
        time_limit_seconds=15.0,
        random_seed=42,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: steps.append((step, score.hard)),
    )

    assert schedule.score.is_feasible()
    assert schedule.score.hard == 0
    assert steps  # on_new_best fired

    # 전 작업 배정 + 배정값이 값 범위 안.
    assignment = schedule.assignment()
    assert all(0 <= v < schedule.crew_date_count() for v in assignment)

    # hard 0 검증: crew 별로 [start, end) 가 겹치지 않고, min/max 창 안.
    by_crew: dict[int, list[tuple[int, int]]] = {}
    for i, job in enumerate(schedule.jobs):
        start = schedule.job_start_date(i)
        end = schedule.job_end_date(i)
        assert start is not None and end is not None
        assert end == mnt.plus_business_days(start, job.duration_in_days)
        assert job.min_start_date <= start
        assert end <= job.max_end_date
        by_crew.setdefault(schedule.crew_index_of(job.crew_date), []).append((start, end))
    for intervals in by_crew.values():
        intervals.sort()
        for (s1, e1), (s2, _) in pairwise(intervals):
            assert e1 <= s2, (s1, e1, s2)


def test_pinned_job_is_kept():
    schedule = mnt.generate_demo_data("SMALL", MONDAY)
    # job 0 을 자기 윈도우 안 첫 시작일, Gamma crew 에 고정한다.
    job0 = schedule.jobs[0]
    starts = schedule.start_dates()
    date_index = next(
        i
        for i, epoch in enumerate(starts)
        if epoch >= job0.min_start_date and mnt.plus_business_days(epoch, job0.duration_in_days) <= job0.max_end_date
    )
    pinned_value = schedule.crew_date_of(date_index, 2)
    jobs = schedule.jobs
    jobs[0].crew_date = pinned_value
    jobs[0].pinned = True
    schedule.jobs = jobs

    mnt.solve(schedule, time_limit_seconds=5.0, random_seed=7, stop_at_first_feasible=True)

    assert schedule.jobs[0].crew_date == pinned_value
    assert schedule.score.hard == 0


def test_solver_config_roundtrip():
    config = mnt.SolverConfig()
    config.spent_limit_seconds = 1.5
    config.random_seed = 11
    config.parallel_solver_count = 2
    config.best_score_limit = (0, -1e18)
    assert config.spent_limit_seconds == 1.5
    assert config.best_score_limit == (0, -1e18)

    schedule = mnt.generate_demo_data("SMALL", MONDAY)
    mnt.solve(schedule, config=config)
    assert schedule.score.hard is not None
