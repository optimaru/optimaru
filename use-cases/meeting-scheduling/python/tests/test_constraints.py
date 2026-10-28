"""Constraint spot checks: reproduces representative cases of Java
MeetingSchedulingConstraintProviderTest with compute_breakdown."""

import meeting_scheduling as ms


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


def test_room_conflict():
    # Java roomConflictPenalized: dur 4 @0 and dur 4 @2, same room -> 2.
    s = make_schedule(1, 12, [5, 5], [(4, [], [], 0, 0), (4, [], [], 2, 0)])
    b = ms.compute_breakdown(s)
    assert b.room_conflict_grains == 2
    assert b.overlapping_meeting_grains == 2
    # Placed back to back (dur 4 @0, @4): room conflict 0, one adjacent pair.
    s = make_schedule(1, 12, [5, 5], [(4, [], [], 0, 0), (4, [], [], 4, 0)])
    b = ms.compute_breakdown(s)
    assert b.room_conflict_grains == 0
    assert b.consecutive_meeting_pairs == 1


def test_avoid_overtime():
    # Only grains 0..3 exist; dur 4 @2 has no last index 5 -> 5.
    s = make_schedule(1, 4, [5], [(4, [], [], 2, 0)])
    assert ms.compute_breakdown(s).overtime_sum == 5
    s = make_schedule(1, 4, [5], [(4, [], [], 0, 0)])
    assert ms.compute_breakdown(s).overtime_sum == 0


def test_attendance_conflicts():
    # Java requiredAttendanceConflictPenalized: overlap 2.
    s = make_schedule(1, 12, [5, 5], [(4, [0], [], 0, 0), (4, [0], [], 2, 1)])
    assert ms.compute_breakdown(s).required_conflict_grains == 2
    # Java requiredAndPreferredAttendanceConflictPenalized: full overlap 4.
    s = make_schedule(1, 12, [5, 5], [(4, [0], [], 0, 0), (4, [], [0], 0, 1)])
    assert ms.compute_breakdown(s).required_preferred_conflict_grains == 4
    # Java preferredAttendanceConflictPenalized: full overlap 4.
    s = make_schedule(1, 12, [5, 5], [(4, [], [0], 0, 0), (4, [], [0], 0, 1)])
    assert ms.compute_breakdown(s).preferred_conflict_grains == 4


def test_room_capacity_and_larger_rooms():
    # Java requiredRoomCapacityPenalized: 1 required + 1 preferred > capacity 1 -> 1.
    s = make_schedule(1, 12, [1], [(4, [0], [1], 0, 0)])
    assert ms.compute_breakdown(s).room_capacity_shortage == 1
    # Java assignLargerRoomsFirstPenalized: uses a capacity-1 room while a 2-seat room exists -> 1.
    s = make_schedule(1, 12, [1, 2], [(4, [], [], 0, 0)])
    assert ms.compute_breakdown(s).larger_room_capacity_sum == 1


def test_room_stability():
    # Java roomStabilityPenalized: same attendee, different room, gap of 0 grains -> 1.
    s = make_schedule(1, 16, [5, 5], [(4, [0], [], 0, 0), (4, [0], [], 4, 1)])
    assert ms.compute_breakdown(s).room_stability_violations == 1
    # Java roomStabilityUnpenalized: gap of 4 grains -> 0.
    s = make_schedule(1, 16, [5, 5], [(4, [0], [], 0, 0), (4, [0], [], 8, 1)])
    assert ms.compute_breakdown(s).room_stability_violations == 0


def test_score_mapping():
    # hard = -(sum of hard), soft = -(medium * 1e6 + raw soft).
    s = make_schedule(1, 12, [5, 5], [(4, [0], [1], 0, 0), (4, [0], [1], 2, 0)])
    b = ms.compute_breakdown(s)
    score = ms.calculate_score(s)
    assert score.hard == -b.hard_total()
    assert score.soft == -(b.medium_total() * ms.MEDIUM_SCALE + b.soft_total())
    assert b.medium_total() > 0  # medium really got folded


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
    try:
        ms.calculate_score(s)
        raise AssertionError("duplicate assignment was not rejected")
    except ValueError:
        pass
