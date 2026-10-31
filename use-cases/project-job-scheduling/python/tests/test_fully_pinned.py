"""A fully pinned schedule comes back unchanged, and most-pinned keeps its pins."""

import project_job as pj
from project_job.demo import generate_demo_data


def _reversed_values(pin):
    schedule = generate_demo_data().to_core()
    pj.solve(schedule, time_limit_seconds=0.1, random_seed=1)
    values = [allocation.mode_delay for allocation in schedule.allocations][::-1]
    for index, allocation in enumerate(schedule.allocations):
        allocation.mode_delay = values[index]
        allocation.pinned = pin(index)
    return schedule, values


def test_a_fully_pinned_schedule_is_not_moved():
    schedule, values = _reversed_values(lambda index: True)
    pj.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [allocation.mode_delay for allocation in schedule.allocations] == values


def test_every_pin_holds_with_one_free_allocation():
    schedule, values = _reversed_values(lambda index: index != 1)
    pj.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [allocation.mode_delay for allocation in schedule.allocations][2:] == values[2:]
