"""A fully pinned schedule comes back unchanged, and most-pinned keeps its pins."""

import pytest

import maintenance_scheduling as mnt


def _reversed_values(pin):
    schedule = mnt.generate_demo_data("SMALL")
    mnt.solve(schedule, time_limit_seconds=0.1, random_seed=1)
    values = [job.crew_date for job in schedule.jobs][::-1]
    for index, job in enumerate(schedule.jobs):
        job.crew_date = values[index]
        job.pinned = pin(index)
    return schedule, values


def test_a_fully_pinned_schedule_is_not_moved():
    schedule, values = _reversed_values(lambda index: True)
    mnt.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [job.crew_date for job in schedule.jobs] == values


def test_every_pin_holds_with_one_free_job():
    schedule, values = _reversed_values(lambda index: index != 0)
    mnt.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [job.crew_date for job in schedule.jobs][1:] == values[1:]


def test_a_pinned_entity_without_a_value_is_refused():
    schedule, _ = _reversed_values(lambda index: index == 0)
    jobs = schedule.jobs
    jobs[0].crew_date = -1
    schedule.jobs = jobs
    with pytest.raises(ValueError, match="is pinned but has no value"):
        mnt.solve(schedule, time_limit_seconds=0.1)
