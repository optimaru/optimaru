"""Constraint spot checks: representative hand-computed cases through the
Python bindings.

The raw costs come from `constraint_costs()` (summed over the units of each
constraint), the per-constraint surface.
"""

import pytest

import optimaru.meeting_scheduling as ms
from optimaru.meeting_scheduling.constraints import get_constraints, score


def raw_totals(schedule):
    """Raw cost per constraint name (the units of each constraint summed)."""
    return {name: sum(entry["costs"]) for name, entry in ms.constraint_costs(schedule).items()}


def level_totals(schedule):
    """Raw totals summed per default level (hard, medium, soft), at the default weights."""
    totals = {"hard": 0.0, "medium": 0.0, "soft": 0.0}
    for entry in ms.constraint_costs(schedule).values():
        totals[entry["level"]] += entry["weight"] * sum(entry["costs"])
    return totals


def make_base(days: int, grains_per_day: int, capacities: list[int]) -> tuple[list, list, list]:
    people = [ms.Person(str(i)) for i in range(4)]
    grains = [
        ms.TimeGrain(
            str(d * grains_per_day + g),
            grain_index=d * grains_per_day + g,
            day_of_year=100 + d,
            starting_minute_of_day=8 * 60 + 15 * g,
        )
        for d in range(days)
        for g in range(grains_per_day)
    ]
    rooms = [ms.Room(f"R{i}", capacity=c) for i, c in enumerate(capacities)]
    return people, grains, rooms


def make_schedule(days, grains_per_day, capacities, meetings_spec):
    """meetings_spec: (duration, required, preferred, grain_pos|None, room)."""
    people, grains, rooms = make_base(days, grains_per_day, capacities)
    meetings = []
    assignments = []
    for i, (duration, required, preferred, grain_pos, room) in enumerate(meetings_spec):
        meetings.append(
            ms.Meeting(
                str(i),
                duration_in_grains=duration,
                required_attendees=required,
                preferred_attendees=preferred,
            )
        )
        grain_room = -1 if grain_pos is None else grain_pos * len(rooms) + room
        assignments.append(ms.MeetingAssignment(str(i), i, grain_room=grain_room))
    return ms.MeetingSchedule(people, grains, rooms, meetings, assignments)


def test_room_conflict_cost():
    # Room conflict: dur 4 @0 and dur 4 @2, same room -> 2.
    s = make_schedule(1, 12, [5, 5], [(4, [], [], 0, 0), (4, [], [], 2, 0)])
    raw = raw_totals(s)
    assert raw["room_conflict_cost"] == 2
    assert raw["meeting_overlap_cost"] == 2
    # Placed back to back (dur 4 @0, @4): room conflict 0, one adjacent pair.
    s = make_schedule(1, 12, [5, 5], [(4, [], [], 0, 0), (4, [], [], 4, 0)])
    raw = raw_totals(s)
    assert raw["room_conflict_cost"] == 0
    assert raw["consecutive_meetings_cost"] == 1


def test_overtime_cost():
    # Only grains 0..3 exist; dur 4 @2 has no last index 5 -> 5.
    s = make_schedule(1, 4, [5], [(4, [], [], 2, 0)])
    assert raw_totals(s)["overtime_cost"] == 5
    s = make_schedule(1, 4, [5], [(4, [], [], 0, 0)])
    assert raw_totals(s)["overtime_cost"] == 0


def test_attendance_conflicts():
    # Shared required attendee: overlap 2.
    s = make_schedule(1, 12, [5, 5], [(4, [0], [], 0, 0), (4, [0], [], 2, 1)])
    assert raw_totals(s)["required_attendance_conflict_cost"] == 2
    # Required in one meeting, preferred in the other: full overlap 4.
    s = make_schedule(1, 12, [5, 5], [(4, [0], [], 0, 0), (4, [], [0], 0, 1)])
    assert raw_totals(s)["required_and_preferred_attendance_conflict_cost"] == 4
    # Shared preferred attendee: full overlap 4.
    s = make_schedule(1, 12, [5, 5], [(4, [], [0], 0, 0), (4, [], [0], 0, 1)])
    assert raw_totals(s)["preferred_attendance_conflict_cost"] == 4


def test_room_capacity_and_larger_rooms():
    # Room too small: 1 required + 1 preferred > capacity 1 -> 1.
    s = make_schedule(1, 12, [1], [(4, [0], [1], 0, 0)])
    assert raw_totals(s)["room_capacity_cost"] == 1
    # Larger room left unused: uses a capacity-1 room while a 2-seat room exists -> 1.
    s = make_schedule(1, 12, [1, 2], [(4, [], [], 0, 0)])
    assert raw_totals(s)["larger_room_capacity_cost"] == 1


def test_room_change_cost():
    # Room change: same attendee, different room, gap of 0 grains -> 1.
    s = make_schedule(1, 16, [5, 5], [(4, [0], [], 0, 0), (4, [0], [], 4, 1)])
    assert raw_totals(s)["room_change_cost"] == 1
    # No room change counted: gap of 4 grains -> 0.
    s = make_schedule(1, 16, [5, 5], [(4, [0], [], 0, 0), (4, [0], [], 8, 1)])
    assert raw_totals(s)["room_change_cost"] == 0


def test_score_mapping():
    # hard = -(sum of hard), soft = -(medium * 1e6 + raw soft).
    s = make_schedule(1, 12, [5, 5], [(4, [0], [1], 0, 0), (4, [0], [1], 2, 0)])
    levels = level_totals(s)
    total = ms.calculate_score(s)
    assert total.hard == -levels["hard"]
    assert total.soft == -(levels["medium"] * ms.MEDIUM_SCALE + levels["soft"])
    assert levels["medium"] > 0  # medium really got folded
    # The pure-Python score folds the same way (medium_fold on every entry).
    assert score(ms.constraint_costs(s), get_constraints()) == (total.hard, total.soft)


def test_unassigned_contributes_nothing():
    s = make_schedule(1, 12, [1], [(4, [0, 1, 2], [], None, 0)])
    score = ms.calculate_score(s)
    assert score.hard == 0
    assert score.soft == 0.0


def test_duplicate_assignment_rejected():
    # Violating meeting:assignment 1:1 is a ValueError (std::invalid_argument).
    people, grains, rooms = make_base(1, 12, [5])
    meetings = [ms.Meeting("0", duration_in_grains=4)]
    assignments = [ms.MeetingAssignment("0", 0), ms.MeetingAssignment("1", 0)]
    s = ms.MeetingSchedule(people, grains, rooms, meetings, assignments)
    with pytest.raises(ValueError):
        ms.calculate_score(s)
