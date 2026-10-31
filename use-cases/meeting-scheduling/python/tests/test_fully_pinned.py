"""A fully pinned schedule comes back unchanged, and most-pinned keeps its pins."""

import pytest

import meeting_scheduling as ms


def _reversed_values(pin):
    schedule = ms.demo_data()
    ms.solve(schedule, time_limit_seconds=0.1, random_seed=1)
    values = [assignment.grain_room for assignment in schedule.meeting_assignments][::-1]
    for index, assignment in enumerate(schedule.meeting_assignments):
        assignment.grain_room = values[index]
        assignment.pinned = pin(index)
    return schedule, values


def test_a_fully_pinned_schedule_is_not_moved():
    schedule, values = _reversed_values(lambda index: True)
    ms.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [assignment.grain_room for assignment in schedule.meeting_assignments] == values


def test_every_pin_holds_with_one_free_meeting():
    schedule, values = _reversed_values(lambda index: index != 0)
    ms.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [assignment.grain_room for assignment in schedule.meeting_assignments][1:] == values[1:]


def test_a_pinned_entity_without_a_value_is_refused():
    schedule, _ = _reversed_values(lambda index: index == 0)
    schedule.meeting_assignments[0].grain_room = -1
    with pytest.raises(ValueError, match="is pinned but has no value"):
        ms.solve(schedule, time_limit_seconds=0.1)
