"""제약 스팟체크: C++ 테스트의 손계산 케이스를 _core 로 재검증한다."""

import pytest

import food_packaging as fp

BIG = 1_000_000_000
PA_S, PA_L, PB = 0, 1, 2  # Java 테스트의 제품 3종 (A large 는 행렬 유지용)


def make_schedule(operator_count, lines, loose_jobs=(), pooled_jobs=()):
    """Java ConstraintProviderTest 의 제품/청소 행렬로 스케줄을 만든다.

    lines: [(operator_index, [(product, duration, ideal_end, max_end), ...])]
    """
    products = [fp.Product("1", "Product A small"), fp.Product("2", "Product A large"), fp.Product("3", "Product B")]
    cleanup = [
        0, 0, 40,  # previous: A small
        5, 0, 40,  # previous: A large
        60, 60, 0,  # previous: B
    ]  # fmt: skip
    jobs = []
    core_lines = []

    def add_job(spec):
        product, duration, ideal, max_end = spec
        index = len(jobs)
        jobs.append(fp.Job(str(index), f"job{index}", product, duration, 0, ideal, max_end))
        return index

    for operator_index, specs in lines:
        indices = [add_job(spec) for spec in specs]
        core_lines.append(fp.Line(str(len(core_lines)), f"Line {len(core_lines)}", 0, operator_index, indices))
    for spec in loose_jobs:
        add_job(spec)
    pool = [add_job(spec) for spec in pooled_jobs]
    operators = [fp.Operator(f"Operator {o + 1}") for o in range(operator_count)]
    return fp.PackagingSchedule(0, 14 * 24 * 60, products, cleanup, operators, core_lines, jobs, pool)


def test_max_end_date_time():
    # J1 은 200 에 딱 맞게, J2 는 350 에 끝나 maxEnd 250 대비 100분 지각.
    schedule = make_schedule(1, [(0, [(PA_S, 200, BIG, 200), (PA_S, 150, BIG, 250)])])
    assert fp.calculate_score(schedule) == fp.Score(-100, 0, -(350 * 350))
    assert fp.analyze(schedule).max_end_date_time == 100


def test_cleanup_chain_shadows():
    # B -> A small 청소 60분: J1 은 [100,160) 청소 후 160..260 생산.
    schedule = make_schedule(1, [(0, [(PB, 100, BIG, BIG), (PA_S, 100, BIG, BIG)])])
    schedule.update_shadow_variables()
    j1 = schedule.jobs[1]
    assert j1.line_index == 0
    assert (j1.start_cleaning_minute, j1.start_production_minute, j1.end_minute) == (100, 160, 260)
    j0 = schedule.jobs[0]
    assert (j0.start_cleaning_minute, j0.start_production_minute, j0.end_minute) == (0, 0, 100)


def test_operator_cleaning_conflict():
    # 라인 0/1 이 operator 0 공유: 청소 구간 [10,70) 과 [30,90) 이 40분 겹침.
    schedule = make_schedule(
        2,
        [
            (0, [(PB, 10, BIG, BIG), (PA_S, 100, BIG, BIG)]),
            (0, [(PB, 30, BIG, BIG), (PA_S, 100, BIG, BIG)]),
            (1, [(PB, 5, BIG, BIG), (PA_S, 100, BIG, BIG)]),
        ],
    )
    breakdown = fp.analyze(schedule)
    assert breakdown.operator_cleaning_conflict == 40
    assert breakdown.minimize_makespan == 170 * 170 + 190 * 190 + 165 * 165


def test_maximize_jobs_assigned():
    # 루즈 작업(리스트 밖)과 풀에 담긴 작업 모두 미배정으로 계산된다.
    schedule = make_schedule(
        1,
        [(0, [(PA_S, 100, BIG, BIG)])],
        loose_jobs=[(PA_S, 200, BIG, BIG)],
        pooled_jobs=[(PB, 50, BIG, BIG)],
    )
    assert fp.calculate_score(schedule) == fp.Score(0, -250, -(100 * 100))


def test_minimize_makespan():
    # 라인 0: 100분. 라인 1: 200 + 청소 40 + 1000 = 1240 에 종료.
    schedule = make_schedule(
        2,
        [(0, [(PA_S, 100, BIG, BIG)]), (1, [(PA_S, 200, BIG, BIG), (PB, 1000, BIG, BIG)])],
        loose_jobs=[(PA_S, 6000, BIG, BIG)],
    )
    assert fp.calculate_score(schedule) == fp.Score(0, -6000, -(100 * 100 + 1240 * 1240))


def test_missing_cleanup_raises_like_java():
    schedule = make_schedule(1, [(0, [(PB, 100, BIG, BIG), (PA_S, 100, BIG, BIG)])])
    cleanup = schedule.cleanup_minutes
    cleanup[PB * 3 + PA_S] = -1  # 항목 제거 (Java: map 에 키 없음)
    schedule.cleanup_minutes = cleanup
    with pytest.raises(ValueError, match="Cleanup duration"):
        fp.calculate_score(schedule)


def test_operator_helpers():
    # 라인 0/1 공유 청소 구간이 58분 겹침; 힐클라임이 분리해 0 으로.
    schedule = make_schedule(
        2,
        [
            (0, [(PB, 10, BIG, BIG), (PA_S, 100, BIG, BIG)]),
            (0, [(PB, 12, BIG, BIG), (PA_S, 100, BIG, BIG)]),
            (1, [(PB, 300, BIG, BIG), (PA_S, 100, BIG, BIG)]),
        ],
    )
    assert fp.analyze(schedule).operator_cleaning_conflict == 58
    assert fp.improve_operator_assignment(schedule) is True
    assert fp.analyze(schedule).operator_cleaning_conflict == 0
