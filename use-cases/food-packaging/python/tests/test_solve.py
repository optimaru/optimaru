"""데모 solve 검증: Java 데모 인스턴스가 feasible 에 도달하고 배정이
일관적인지, pinned 작업과 unassigned pool 이 지켜지는지."""

import food_packaging as fp


def test_demo_data_matches_java_shape():
    schedule = fp.generate_demo_data()
    assert len(schedule.products) == 60  # 10 ingredients x 6
    assert schedule.line_count() == 5
    assert len(schedule.operators) == 5
    assert len(schedule.jobs) == 100
    assert schedule.to_minute - schedule.from_minute == 14 * 24 * 60

    names = [job.name for job in schedule.jobs]
    assert names == sorted(names)  # Java: jobs.sort(comparing(getName))
    for job in schedule.jobs:
        assert 120 <= job.duration_minutes < 300
        assert job.min_start_minute % (24 * 60) == 0
        assert job.ideal_end_minute % (24 * 60) == 16 * 60  # 16:00
        assert job.max_end_minute >= job.ideal_end_minute + 24 * 60

    count = len(schedule.products)
    for prev in range(count):
        assert schedule.cleanup_of(prev, prev) == 0
        for cur in range(count):
            if prev != cur:
                assert schedule.cleanup_of(prev, cur) == 10 or 30 <= schedule.cleanup_of(prev, cur) < 60

    small = fp.generate_demo_data(line_count=2, job_count=10)
    assert small.line_count() == 2
    assert len(small.jobs) == 10


def test_demo_solve_reaches_feasible():
    schedule = fp.generate_demo_data()
    steps = []
    fp.solve(
        schedule,
        time_limit_seconds=15.0,
        random_seed=42,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: steps.append((step, score.hard)),
    )

    assert schedule.score.is_feasible()
    assert schedule.score.hard == 0
    assert steps  # on_new_best fired

    # 배정 일관성: 모든 작업이 정확히 한 리스트에 있고, shadow 체인이
    # 순차적이며, hard 제약이 실제로 만족된다.
    membership = [0] * len(schedule.jobs)
    for line in schedule.lines:
        for j in line.jobs:
            membership[j] += 1
    for j in schedule.unassigned_jobs:
        membership[j] += 1
    assert all(m == 1 for m in membership)

    jobs = schedule.jobs
    for line_index, line in enumerate(schedule.lines):
        previous_end = line.start_minute
        for j in line.jobs:
            job = jobs[j]
            assert job.line_index == line_index
            assert job.start_cleaning_minute == previous_end
            assert job.start_production_minute >= job.start_cleaning_minute
            assert job.end_minute == job.start_production_minute + job.duration_minutes
            assert job.end_minute <= job.max_end_minute  # hard 0
            previous_end = job.end_minute

    # 데모는 operator 5 : line 5 이므로 단사 배정이어야 충돌 0 이 보장된다.
    operator_indices = [line.operator_index for line in schedule.lines]
    assert sorted(operator_indices) == [0, 1, 2, 3, 4]


def test_pinned_jobs_are_kept():
    schedule = fp.generate_demo_data(line_count=3, job_count=18)
    # 작업 0 을 라인 0 의 맨 앞에 고정하고, 작업 1 은 미배정으로 고정한다.
    jobs = schedule.jobs
    jobs[0].pinned = True
    jobs[1].pinned = True
    schedule.jobs = jobs
    lines = schedule.lines
    lines[0].jobs = [0]
    schedule.lines = lines

    fp.solve(schedule, time_limit_seconds=3.0, random_seed=7)

    assert schedule.lines[0].jobs[0] == 0
    assert schedule.jobs[0].line_index == 0
    assert schedule.jobs[1].line_index == -1
    assert 1 in schedule.unassigned_jobs
    # pinned 미배정 작업의 duration 이 medium 페널티에 남는다.
    assert fp.analyze(schedule).maximize_jobs_assigned >= schedule.jobs[1].duration_minutes


def test_solver_config_roundtrip():
    config = fp.SolverConfig()
    config.spent_limit_seconds = 1.5
    config.random_seed = 11
    config.parallel_solver_count = 2
    config.operator_rounds = 2
    config.best_score_limit = (0, -1e18)
    assert config.spent_limit_seconds == 1.5
    assert config.parallel_solver_count == 2
    assert config.operator_rounds == 2
    assert config.best_score_limit == (0, -1e18)

    schedule = fp.generate_demo_data(line_count=2, job_count=8)
    fp.solve(schedule, config=config)
    assert schedule.score == fp.calculate_score(schedule)
