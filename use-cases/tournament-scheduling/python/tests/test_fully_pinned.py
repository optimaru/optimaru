"""A fully pinned schedule comes back unchanged, and most-pinned keeps its pins."""

import pytest

import tournament_scheduling as ts
from tournament_scheduling.models import demo_data


def _reversed_values(pin):
    schedule = demo_data().to_core()
    ts.solve(schedule, time_limit_seconds=0.1, random_seed=1)
    values = [assignment.team for assignment in schedule.team_assignments][::-1]
    for index, assignment in enumerate(schedule.team_assignments):
        assignment.team = values[index]
        assignment.pinned = pin(index)
    return schedule, values


def test_a_fully_pinned_schedule_is_not_moved():
    schedule, values = _reversed_values(lambda index: True)
    ts.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [assignment.team for assignment in schedule.team_assignments] == values


def test_every_pin_holds_with_one_free_slot():
    schedule, values = _reversed_values(lambda index: index != 0)
    ts.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [assignment.team for assignment in schedule.team_assignments][1:] == values[1:]


def test_a_pinned_entity_without_a_value_is_refused():
    schedule, _ = _reversed_values(lambda index: index == 0)
    schedule.team_assignments[0].team = -1
    with pytest.raises(ValueError, match="is pinned but has no value"):
        ts.solve(schedule, time_limit_seconds=0.1)
