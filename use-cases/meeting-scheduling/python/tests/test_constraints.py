"""제약 스팟체크: Java MeetingSchedulingConstraintProviderTest 의 대표
케이스를 compute_breakdown 으로 재현한다."""

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
    # Java roomConflictPenalized: dur 4 @0 과 dur 4 @2, 같은 방 -> 2.
    s = make_schedule(1, 12, [5, 5], [(4, [], [], 0, 0), (4, [], [], 2, 0)])
    b = ms.compute_breakdown(s)
    assert b.room_conflict_grains == 2
    assert b.overlapping_meeting_grains == 2
    # 붙여 배치하면 (dur 4 @0, @4) 방 충돌 0, 연속 쌍 1.
    s = make_schedule(1, 12, [5, 5], [(4, [], [], 0, 0), (4, [], [], 4, 0)])
    b = ms.compute_breakdown(s)
    assert b.room_conflict_grains == 0
    assert b.consecutive_meeting_pairs == 1


def test_avoid_overtime():
    # 그레인 0..3 만 존재; dur 4 @2 는 마지막 인덱스 5 가 없다 -> 5.
    s = make_schedule(1, 4, [5], [(4, [], [], 2, 0)])
    assert ms.compute_breakdown(s).overtime_sum == 5
    s = make_schedule(1, 4, [5], [(4, [], [], 0, 0)])
    assert ms.compute_breakdown(s).overtime_sum == 0


def test_attendance_conflicts():
    # Java requiredAttendanceConflictPenalized: 겹침 2.
    s = make_schedule(1, 12, [5, 5], [(4, [0], [], 0, 0), (4, [0], [], 2, 1)])
    assert ms.compute_breakdown(s).required_conflict_grains == 2
    # Java requiredAndPreferredAttendanceConflictPenalized: 완전 겹침 4.
    s = make_schedule(1, 12, [5, 5], [(4, [0], [], 0, 0), (4, [], [0], 0, 1)])
    assert ms.compute_breakdown(s).required_preferred_conflict_grains == 4
    # Java preferredAttendanceConflictPenalized: 완전 겹침 4.
    s = make_schedule(1, 12, [5, 5], [(4, [], [0], 0, 0), (4, [], [0], 0, 1)])
    assert ms.compute_breakdown(s).preferred_conflict_grains == 4


def test_room_capacity_and_larger_rooms():
    # Java requiredRoomCapacityPenalized: 필수 1 + 선호 1 > 수용량 1 -> 1.
    s = make_schedule(1, 12, [1], [(4, [0], [1], 0, 0)])
    assert ms.compute_breakdown(s).room_capacity_shortage == 1
    # Java assignLargerRoomsFirstPenalized: 수용량 1 방 사용, 2 인 방 존재 -> 1.
    s = make_schedule(1, 12, [1, 2], [(4, [], [], 0, 0)])
    assert ms.compute_breakdown(s).larger_room_capacity_sum == 1


def test_room_stability():
    # Java roomStabilityPenalized: 같은 참석자, 다른 방, 간격 0 그레인 -> 1.
    s = make_schedule(1, 16, [5, 5], [(4, [0], [], 0, 0), (4, [0], [], 4, 1)])
    assert ms.compute_breakdown(s).room_stability_violations == 1
    # Java roomStabilityUnpenalized: 간격 4 그레인 -> 0.
    s = make_schedule(1, 16, [5, 5], [(4, [0], [], 0, 0), (4, [0], [], 8, 1)])
    assert ms.compute_breakdown(s).room_stability_violations == 0


def test_score_mapping():
    # hard = -(hard 합), soft = -(medium * 1e6 + raw soft).
    s = make_schedule(1, 12, [5, 5], [(4, [0], [1], 0, 0), (4, [0], [1], 2, 0)])
    b = ms.compute_breakdown(s)
    score = ms.calculate_score(s)
    assert score.hard == -b.hard_total()
    assert score.soft == -(b.medium_total() * ms.MEDIUM_SCALE + b.soft_total())
    assert b.medium_total() > 0  # medium 이 실제로 접혔는지


def test_unassigned_contributes_nothing():
    s = make_schedule(1, 12, [1], [(4, [0, 1, 2], [], None, 0)])
    score = ms.calculate_score(s)
    assert score.hard == 0
    assert score.soft == 0.0


def test_duplicate_assignment_rejected():
    # 미팅:배정 1:1 위반은 ValueError (std::invalid_argument).
    people, grains, rooms = make_base(1, 12, [5])
    meetings = [ms.Meeting("0", duration_in_grains=4)]
    assignments = [ms.MeetingAssignment("0", 0), ms.MeetingAssignment("1", 0)]
    s = ms.MeetingSchedule(people, grains, rooms, meetings, assignments)
    try:
        ms.calculate_score(s)
        raise AssertionError("중복 배정이 거부되지 않았다")
    except ValueError:
        pass
