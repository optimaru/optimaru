"""제약 스팟체크: calculate_score 를 손계산 케이스와 대조한다 (Java
MaintenanceSchedulingConstraintProviderTest 의 대표 케이스; 전수 이식은
cpp/test/mnt_test.cpp 에 있다)."""

import maintenance_scheduling as mnt

# 2021-02-01 (월요일) == epoch day 18659, Java 테스트의 DAY_1.
DAY_1 = 18659
DAY_2 = DAY_1 + 1
DAY_3 = DAY_1 + 2


def schedule_of(jobs_and_assignments):
    """[(Job, crew_index, start_epoch|None)] -> 채점된 MaintenanceSchedule."""
    jobs = [job for job, _, _ in jobs_and_assignments]
    schedule = mnt.MaintenanceSchedule(
        DAY_1,
        DAY_1 + 28,
        [mnt.Crew("1", "Alpha crew"), mnt.Crew("2", "Beta crew")],
        jobs,
    )
    date_index = {epoch: i for i, epoch in enumerate(schedule.start_dates())}
    assignment = []
    for _, crew_index, start_epoch in jobs_and_assignments:
        if crew_index is None or start_epoch is None:
            assignment.append(-1)
        else:
            assignment.append(schedule.crew_date_of(date_index[start_epoch], crew_index))
    schedule.set_assignment(assignment)
    mnt.calculate_score(schedule)
    return schedule


def test_crew_conflict_overlap_days():
    # [월,목) vs [화,금) 같은 crew: 겹침 화+수 = 2일 -> hard -2.
    schedule = schedule_of(
        [
            (mnt.Job("1", "Downtown tunnel", 3), 0, DAY_1),
            (mnt.Job("2", "Uptown bridge", 3), 0, DAY_2),
        ]
    )
    assert schedule.score.hard == -2
    assert schedule.score.soft == 0

    # 다른 crew 면 겹쳐도 무벌점.
    schedule = schedule_of(
        [
            (mnt.Job("1", "Downtown tunnel", 3), 0, DAY_1),
            (mnt.Job("2", "Uptown bridge", 3), 1, DAY_2),
        ]
    )
    assert schedule.score.hard == 0


def test_min_start_and_max_end():
    # minStartDate 2일 위반 + (dur 1 @ 월) end=화 <= max 수: max 는 무벌점.
    schedule = schedule_of([(mnt.Job("1", "j", 1, min_start_date=DAY_3, max_end_date=DAY_3), 0, DAY_1)])
    assert schedule.score.hard == -2

    # dur 4 @ 월 -> end 금(=DAY_1+4); max 수 보다 2일 늦음.
    schedule = schedule_of([(mnt.Job("1", "j", 4, max_end_date=DAY_3), 0, DAY_1)])
    assert schedule.score.hard == -2


def test_ideal_end_date_soft_weights():
    # dur 1 @ 화 -> end 수; ideal 화 보다 1일 늦음 -> soft -1'000'000.
    schedule = schedule_of([(mnt.Job("1", "j", 1, ideal_end_date=DAY_2), 0, DAY_2)])
    assert schedule.score.hard == 0
    assert schedule.score.soft == -1_000_000

    # dur 0 @ 월 -> end 월; ideal 수 보다 2일 이름 -> soft -2.
    schedule = schedule_of([(mnt.Job("1", "j", 0, ideal_end_date=DAY_3), 0, DAY_1)])
    assert schedule.score.soft == -2


def test_tag_conflict():
    # 같은 날 1일 겹침, 공유 태그 {Downtown} 1개 -> soft -1'000
    # (같은 crew 라 crewConflict hard -1 도 함께).
    schedule = schedule_of(
        [
            (mnt.Job("1", "t", 1, tags=["Downtown"]), 0, DAY_1),
            (mnt.Job("2", "b", 1, tags=["Downtown", "Crane"]), 0, DAY_1),
        ]
    )
    assert schedule.score.hard == -1
    assert schedule.score.soft == -1_000

    # 겹치지만 공유 태그가 없으면 tagConflict 무벌점.
    schedule = schedule_of(
        [
            (mnt.Job("1", "t", 1, tags=["Downtown"]), 0, DAY_1),
            (mnt.Job("2", "b", 1, tags=["Uptown", "Crane"]), 1, DAY_1),
        ]
    )
    assert schedule.score.soft == 0


def test_end_date_skips_weekends():
    # 월요일 + 5 business days = 토요일(exclusive), +6 = 다음 화요일.
    assert mnt.plus_business_days(DAY_1, 5) == DAY_1 + 5
    assert mnt.plus_business_days(DAY_1, 6) == DAY_1 + 8

    schedule = schedule_of([(mnt.Job("1", "j", 6), 0, DAY_1)])
    assert schedule.job_start_date(0) == DAY_1
    assert schedule.job_end_date(0) == DAY_1 + 8


def test_unassigned_contributes_nothing():
    schedule = schedule_of(
        [
            (mnt.Job("1", "j", 9, min_start_date=DAY_3, max_end_date=DAY_1, ideal_end_date=DAY_1), None, None),
        ]
    )
    assert schedule.score.hard == 0
    assert schedule.score.soft == 0
