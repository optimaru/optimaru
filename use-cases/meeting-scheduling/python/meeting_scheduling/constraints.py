"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`ms.solve(schedule)`) enables all twelve constraints at the provider weights.
This module is the alternative path next to it: pick constraints, change weights, flip
hard/soft, and pass the list to `solve(schedule, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import meeting_scheduling as ms
    from meeting_scheduling.constraints import (
        MeetingOverlapCost, get_constraints, room_conflict_cost,
    )

    ms.solve(schedule, constraints=get_constraints())           # defaults as-is
    ms.solve(schedule, constraints=get_constraints([            # picked
        room_conflict_cost, MeetingOverlapCost(weight=100),
    ]))

Each constraint has a default instance at module level, so you can also pass a
list of just a few. Only listed constraints are scored — leave one out and its
term disappears from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost, in units of its level.
               ``None`` = the C++ component default (``ms.constraint_info()``
               lists them).
- ``hard``     ``True`` = hard, ``False`` = soft, ``None`` = the default level.
               The two attendance conflicts that are not both-required
               (`required_and_preferred_attendance_conflict_cost`,
               `preferred_attendance_conflict_cost`) are **medium**: folded into
               the engine's soft level as ``medium * 1e6`` (``MEDIUM_SCALE``).
               Medium is a default-only level: ``hard=True`` charges one hard
               unit per person-grain, ``hard=False`` one soft unit per
               person-grain, and an explicit ``weight`` stays in medium units.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``, per (grain, room) cell for the five per-meeting
               constraints and on the total for the seven pair constraints.
               2.0 = squared, so concentrated violations grow much costlier.

Attendance conflicts split into three terms: hard when both are **required**, medium
when only one is required or both are preferred — levels separate impossible schedules
from merely regrettable ones.

Names (`name`) are the C++ component names in snake_case
(``RoomConflictCost`` -> ``room_conflict_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`ms.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

from ._constraint import Constraint, Score, score_costs, select

__all__ = [
    "Constraint",
    "Score",
    "score",
    "OvertimeCost",
    "RoomCapacityCost",
    "DayCrossingCost",
    "MeetingEndTimeCost",
    "LargerRoomCapacityCost",
    "RoomConflictCost",
    "RequiredAttendanceConflictCost",
    "RequiredAndPreferredAttendanceConflictCost",
    "PreferredAttendanceConflictCost",
    "ConsecutiveMeetingsCost",
    "MeetingOverlapCost",
    "RoomChangeCost",
    "overtime_cost",
    "room_capacity_cost",
    "day_crossing_cost",
    "meeting_end_time_cost",
    "larger_room_capacity_cost",
    "room_conflict_cost",
    "required_attendance_conflict_cost",
    "required_and_preferred_attendance_conflict_cost",
    "preferred_attendance_conflict_cost",
    "consecutive_meetings_cost",
    "meeting_overlap_cost",
    "room_change_cost",
    "get_constraints",
]


class OvertimeCost(Constraint):
    """Meetings ending past the end of the calendar. Default: hard, weight 1.

    The case where no TimeGrain fact exists at the last grain index. The raw cost is
    that index (the Java provider's weigher, kept as it is), not the overrun, so
    running further over costs more."""

    name = "overtime_cost"
    label_en = "Overtime"
    label_ko = "달력 초과"
    description_en = (
        "Meetings that run past the end of the calendar (no time grain exists at their last grain index). "
        "Such a meeting costs its last grain index, not the length of the overrun, so running further over "
        "costs more; 0 means every meeting ends on an existing time grain."
    )
    description_ko = (
        "달력 끝을 넘겨 끝나는 회의(마지막 그레인 인덱스에 해당하는 시간 그레인이 없는 회의)를 셉니다. "
        "이런 회의는 넘친 길이가 아니라 마지막 그레인 인덱스만큼 비용이 들어 더 멀리 넘을수록 비싸집니다. "
        "0이면 모든 회의가 있는 시간 그레인에서 끝납니다."
    )


class RoomCapacityCost(Constraint):
    """Seats missing in the assigned room for the attendees. Default: hard, weight 1."""

    name = "room_capacity_cost"
    label_en = "Room capacity"
    label_ko = "회의실 수용 인원"
    description_en = (
        "Seats a room is short for the meeting placed in it: required plus preferred attendees minus the "
        "room's capacity, summed over meetings. One unit of cost is one missing seat; 0 means every meeting "
        "fits its room."
    )
    description_ko = (
        "회의마다 필수·선호 참석자 수가 배정된 회의실의 수용 인원을 넘는 만큼을 모두 더합니다. "
        "모자란 좌석 1개가 비용 1이며, 0이면 모든 회의가 회의실에 들어갑니다."
    )


class DayCrossingCost(Constraint):
    """Meetings whose last grain falls on a different day than the first. Default: hard, weight 1.

    Counted per TimeGrain fact at the last index on another day (the shape of the
    Java join). Running off the calendar entirely gives 0 — `overtime_cost`
    values that."""

    name = "day_crossing_cost"
    label_en = "Day crossing"
    label_ko = "날짜 넘김"
    description_en = (
        "Meetings whose last time grain falls on a different day than their first. One unit of cost is one "
        "time grain at the meeting's last grain index on another day (normally one per crossing meeting); "
        "0 means every meeting starts and ends on the same day. A meeting that runs off the calendar costs 0 "
        "here; `overtime_cost` prices it."
    )
    description_ko = (
        "시작한 날과 다른 날에 마지막 시간 그레인이 놓이는 회의를 셉니다. 회의의 마지막 그레인 인덱스에서 "
        "다른 날에 있는 시간 그레인 하나가 비용 1이며(보통 날을 넘는 회의 하나당 1), 0이면 모든 회의가 같은 "
        "날 시작하고 끝납니다. 달력 밖으로 나가는 회의는 여기서 0이고 `overtime_cost`가 값을 매깁니다."
    )


class MeetingEndTimeCost(Constraint):
    """Each meeting's own last grain index. Default: soft, weight 1.

    Pulls the whole schedule toward the front of the calendar."""

    name = "meeting_end_time_cost"
    label_en = "Meeting end time"
    label_ko = "회의 종료 시점"
    description_en = (
        "The last grain index of every meeting, summed, which pulls the schedule toward the front of the "
        "calendar. One unit of cost is one grain index; 0 only when every meeting ends on grain 0."
    )
    description_ko = (
        "모든 회의의 마지막 그레인 인덱스를 더해 일정을 달력 앞쪽으로 당깁니다. "
        "그레인 인덱스 1이 비용 1이며, 모든 회의가 0번 그레인에서 끝날 때만 0입니다."
    )


class LargerRoomCapacityCost(Constraint):
    """For every room strictly larger than the assigned one, the capacity difference. Default: soft, weight 1.

    Fills big rooms first and leaves small rooms for meetings that only fit there.
    Per-room totals are precomputed by `ScoreCalculator::prepare`. Not a capacity
    overflow (that is `room_capacity_cost`)."""

    name = "larger_room_capacity_cost"
    label_en = "Larger rooms left free"
    label_ko = "남는 큰 회의실"
    description_en = (
        "For each meeting, the capacity of every strictly larger room minus the capacity of its own room, "
        "summed, so the big rooms fill first and the small ones stay free for meetings that only fit there. "
        "One unit of cost is one seat; 0 means the meeting sits in a largest room. This is not a capacity "
        "overflow (see `room_capacity_cost`)."
    )
    description_ko = (
        "회의마다 배정된 회의실보다 큰 회의실 각각과의 수용 인원 차이를 더해, 큰 회의실부터 채우고 작은 "
        "회의실은 그 방에만 맞는 회의에 남겨 둡니다. 좌석 1개가 비용 1이며, 0이면 회의가 가장 큰 회의실에 "
        "있습니다. 수용 인원 초과를 뜻하지 않습니다(`room_capacity_cost` 참고)."
    )


class RoomConflictCost(Constraint):
    """Grains in which two meetings use the same room at the same time. Default: hard, weight 1."""

    name = "room_conflict_cost"
    label_en = "Room conflict"
    label_ko = "회의실 중복 예약"
    description_en = (
        "Two meetings using the same room at the same time: the time grains each such pair overlaps, summed "
        "over the pairs. One unit of cost is one overlapping grain; 0 means no room is double-booked."
    )
    description_ko = (
        "같은 회의실을 같은 시간에 쓰는 두 회의가 겹치는 시간 그레인을 쌍마다 더합니다. "
        "겹치는 그레인 1개가 비용 1이며, 0이면 중복 예약된 회의실이 없습니다."
    )


class RequiredAttendanceConflictCost(Constraint):
    """Overlapping grains of a person **required in both** meetings. Default: hard, weight 1.

    Someone who must be in two meetings at once makes the schedule impossible, so it
    is hard. A meeting listing the same person as required twice folds in here too."""

    name = "required_attendance_conflict_cost"
    label_en = "Required attendance conflict"
    label_ko = "필수 참석자 일정 충돌"
    description_en = (
        "A person required at two meetings that overlap: overlapping grains times the shared required "
        "attendees, summed over meeting pairs. One unit of cost is one person-grain; 0 means no required "
        "attendee is double-booked."
    )
    description_ko = (
        "겹치는 두 회의에 모두 필수로 참석해야 하는 사람에 대해, 겹치는 그레인 수와 공통 필수 참석자 수의 "
        "곱을 회의 쌍마다 더합니다. 한 사람의 1그레인이 비용 1이며, 0이면 일정이 겹친 필수 참석자가 없습니다."
    )


class RequiredAndPreferredAttendanceConflictCost(Constraint):
    """Overlapping grains of a person required in one and preferred in the other. Default: medium, weight 1.

    Both directions count — which is why an edge carries separate forward/backward
    weights and the component adds them. The weight is in medium units; the domain
    folds the medium level into the engine soft level as `medium * 1e6`."""

    name = "required_and_preferred_attendance_conflict_cost"
    label_en = "Required and preferred attendance conflict"
    label_ko = "필수·선호 참석자 일정 충돌"
    description_en = (
        "A person required at one meeting and preferred at another that overlaps it, on the medium level: "
        "overlapping grains times such attendees, in both directions, summed over meeting pairs. One unit "
        "of cost is one person-grain; 0 means no such clash."
    )
    description_ko = (
        "한 회의에는 필수로, 겹치는 다른 회의에는 선호로 참석하는 사람에 대해 겹치는 그레인 수와 그런 참석자 "
        "수의 곱을 양방향으로 회의 쌍마다 더하며, 미디엄 수준에서 매깁니다. 한 사람의 1그레인이 비용 1이며, "
        "0이면 이런 충돌이 없습니다."
    )


class PreferredAttendanceConflictCost(Constraint):
    """Overlapping grains of a person **preferred in both**. Default: medium, weight 1.

    Regrettable rather than impossible, so it sits at medium: below the hard rules
    and above every soft convenience term. The weight is in medium units; the domain
    folds the medium level into the engine soft level as `medium * 1e6`."""

    name = "preferred_attendance_conflict_cost"
    label_en = "Preferred attendance conflict"
    label_ko = "선호 참석자 일정 충돌"
    description_en = (
        "A person who would prefer to attend two meetings that overlap, on the medium level: overlapping "
        "grains times the shared preferred attendees, summed over meeting pairs. One unit of cost is one "
        "person-grain; 0 means no such clash."
    )
    description_ko = (
        "겹치는 두 회의에 모두 선호로 참석하는 사람에 대해 겹치는 그레인 수와 공통 선호 참석자 수의 곱을 "
        "회의 쌍마다 더하며, 미디엄 수준에서 매깁니다. 한 사람의 1그레인이 비용 1이며, 0이면 이런 충돌이 "
        "없습니다."
    )


class ConsecutiveMeetingsCost(Constraint):
    """Two meetings back to back without a single grain between. Default: soft, weight 100.

    The most expensive soft term of the provider — a day packed with gapless meetings
    is what attendees actually complain about. Any two adjacent meetings count,
    whatever their rooms and attendees."""

    name = "consecutive_meetings_cost"
    label_en = "Back-to-back meetings"
    label_ko = "쉬는 시간 없는 연속 회의"
    description_en = (
        "Pairs of meetings scheduled back to back (one ends at the grain the other starts), whatever their "
        "rooms and attendees. One unit of cost is one such pair; 0 means every meeting is followed by a break."
    )
    description_ko = (
        "회의실이나 참석자와 관계없이, 한 회의가 끝나는 그레인에 다른 회의가 바로 시작하는 회의 쌍을 셉니다. "
        "그런 쌍 하나가 비용 1이며, 0이면 모든 회의 사이에 쉬는 시간이 있습니다."
    )


class MeetingOverlapCost(Constraint):
    """Grains in which any two meetings run at once. Default: soft, weight 10.

    Regardless of room or attendees. Spreads the calendar so meetings someone might
    want to attend both of do not overlap."""

    name = "meeting_overlap_cost"
    label_en = "Meeting overlap"
    label_ko = "회의 시간 겹침"
    description_en = (
        "Time grains during which any two meetings run at once, regardless of room or attendees, summed "
        "over the pairs. One unit of cost is one overlapping grain; 0 means no two meetings overlap."
    )
    description_ko = (
        "회의실이나 참석자와 관계없이 두 회의가 동시에 진행되는 시간 그레인을 쌍마다 더합니다. "
        "겹치는 그레인 1개가 비용 1이며, 0이면 시간이 겹치는 회의가 없습니다."
    )


class RoomChangeCost(Constraint):
    """Shared attendees whose next meeting starts within two grains in a different room. Default: soft, weight 1.

    Unlike the three conflict rules, this is not about overlap — it fires
    precisely when meetings do **not** overlap much (people have to pack up and walk)."""

    name = "room_change_cost"
    label_en = "Room change"
    label_ko = "회의실 이동"
    description_en = (
        "Attendees who have to switch rooms between close meetings: for every pair of meetings sharing "
        "attendees where the later one starts in a different room no more than two grains after the earlier "
        "one ends, the number of shared attendances. One unit of cost is one attendee's room switch; 0 means "
        "nobody changes rooms between close meetings."
    )
    description_ko = (
        "참석자를 공유하는 두 회의에서 나중 회의가 앞 회의가 끝난 뒤 2그레인 안에 다른 회의실에서 시작하면 "
        "공유 참석 수만큼 더합니다. 참석자 한 명의 회의실 이동이 비용 1이며, 0이면 가까운 회의 사이에 "
        "회의실을 옮기는 사람이 없습니다."
    )


#: Instances at the default weights (weight=None). Reference them directly when picking.
overtime_cost = OvertimeCost()
room_capacity_cost = RoomCapacityCost()
day_crossing_cost = DayCrossingCost()
meeting_end_time_cost = MeetingEndTimeCost()
larger_room_capacity_cost = LargerRoomCapacityCost()
room_conflict_cost = RoomConflictCost()
required_attendance_conflict_cost = RequiredAttendanceConflictCost()
required_and_preferred_attendance_conflict_cost = RequiredAndPreferredAttendanceConflictCost()
preferred_attendance_conflict_cost = PreferredAttendanceConflictCost()
consecutive_meetings_cost = ConsecutiveMeetingsCost()
meeting_overlap_cost = MeetingOverlapCost()
room_change_cost = RoomChangeCost()

# The default set, in list_constraints() order (unit constraints first, globals after).
_DEFAULTS = (
    overtime_cost,
    room_capacity_cost,
    day_crossing_cost,
    meeting_end_time_cost,
    larger_room_capacity_cost,
    room_conflict_cost,
    required_attendance_conflict_cost,
    required_and_preferred_attendance_conflict_cost,
    preferred_attendance_conflict_cost,
    consecutive_meetings_cost,
    meeting_overlap_cost,
    room_change_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all twelve default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([room_conflict_cost, MeetingOverlapCost(weight=100)])

    Same contract as the C++ applyConstraints: an empty list is an error (pass
    None for the defaults), and naming a constraint twice is an error.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `ms.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = ms.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, picked)             # picked list

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft) — the same as
    `schedule.score`: the medium level (the two non-required attendance
    conflicts) is folded into soft as ``medium * 1e6``, the ``medium_fold``
    every entry of ``costs`` carries.
    """
    return score_costs(costs, get_constraints(constraints))
