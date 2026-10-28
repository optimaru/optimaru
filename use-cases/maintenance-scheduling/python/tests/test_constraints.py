"""Constraint spot checks: calculate_score against hand-computed cases (representative
cases of Java MaintenanceSchedulingConstraintProviderTest; the full port is in
cpp/test/mnt_test.cpp)."""

import maintenance_scheduling as mnt

# 2021-02-01 (Monday) == epoch day 18659, DAY_1 of the Java test.
DAY_1 = 18659
DAY_2 = DAY_1 + 1
DAY_3 = DAY_1 + 2


def schedule_of(jobs_and_assignments):
    """[(Job, crew_index, start_epoch|None)] -> a scored MaintenanceSchedule."""
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
    # [Mon,Thu) vs [Tue,Fri) on the same crew: overlap Tue+Wed = 2 days -> hard -2.
    schedule = schedule_of(
        [
            (mnt.Job("1", "Downtown tunnel", 3), 0, DAY_1),
            (mnt.Job("2", "Uptown bridge", 3), 0, DAY_2),
        ]
    )
    assert schedule.score.hard == -2
    assert schedule.score.soft == 0

    # Different crews overlap without penalty.
    schedule = schedule_of(
        [
            (mnt.Job("1", "Downtown tunnel", 3), 0, DAY_1),
            (mnt.Job("2", "Uptown bridge", 3), 1, DAY_2),
        ]
    )
    assert schedule.score.hard == 0


def test_min_start_and_max_end():
    # minStartDate violated by 2 days + (dur 1 @ Mon) end=Tue <= max Wed: no max penalty.
    schedule = schedule_of([(mnt.Job("1", "j", 1, min_start_date=DAY_3, max_end_date=DAY_3), 0, DAY_1)])
    assert schedule.score.hard == -2

    # dur 4 @ Mon -> end Fri (=DAY_1+4); 2 days later than max Wed.
    schedule = schedule_of([(mnt.Job("1", "j", 4, max_end_date=DAY_3), 0, DAY_1)])
    assert schedule.score.hard == -2


def test_ideal_end_date_soft_weights():
    # dur 1 @ Tue -> end Wed; 1 day later than ideal Tue -> soft -1'000'000.
    schedule = schedule_of([(mnt.Job("1", "j", 1, ideal_end_date=DAY_2), 0, DAY_2)])
    assert schedule.score.hard == 0
    assert schedule.score.soft == -1_000_000

    # dur 0 @ Mon -> end Mon; 2 days earlier than ideal Wed -> soft -2.
    schedule = schedule_of([(mnt.Job("1", "j", 0, ideal_end_date=DAY_3), 0, DAY_1)])
    assert schedule.score.soft == -2


def test_tag_conflict():
    # 1-day overlap on the same day, 1 shared tag {Downtown} -> soft -1'000
    # (same crew, so crewConflict hard -1 as well).
    schedule = schedule_of(
        [
            (mnt.Job("1", "t", 1, tags=["Downtown"]), 0, DAY_1),
            (mnt.Job("2", "b", 1, tags=["Downtown", "Crane"]), 0, DAY_1),
        ]
    )
    assert schedule.score.hard == -1
    assert schedule.score.soft == -1_000

    # Overlapping but without shared tags: no tagConflict penalty.
    schedule = schedule_of(
        [
            (mnt.Job("1", "t", 1, tags=["Downtown"]), 0, DAY_1),
            (mnt.Job("2", "b", 1, tags=["Uptown", "Crane"]), 1, DAY_1),
        ]
    )
    assert schedule.score.soft == 0


def test_end_date_skips_weekends():
    # Monday + 5 business days = Saturday (exclusive), +6 = the next Tuesday.
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
