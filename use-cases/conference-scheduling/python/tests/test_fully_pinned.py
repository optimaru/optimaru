"""A fully pinned schedule comes back unchanged, and most-pinned keeps its pins."""

import conference_scheduling as cs


def _reversed_values(pin):
    schedule = cs.demo_conference()
    cs.solve(schedule, time_limit_seconds=0.1, random_seed=1)
    values = [talk.slot_room for talk in schedule.talks][::-1]
    for index, talk in enumerate(schedule.talks):
        talk.slot_room = values[index]
        talk.pinned = pin(index)
    return schedule, values


def test_a_fully_pinned_schedule_is_not_moved():
    schedule, values = _reversed_values(lambda index: True)
    cs.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [talk.slot_room for talk in schedule.talks] == values


def test_every_pin_holds_with_one_free_talk():
    schedule, values = _reversed_values(lambda index: index != 0)
    cs.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [talk.slot_room for talk in schedule.talks][1:] == values[1:]
