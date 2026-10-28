"""Class-based CP-SAT plugins for meeting scheduling."""

from collections import Counter
from dataclasses import dataclass, field
from typing import Any, ClassVar

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from meeting_scheduling import constraints as domain
from meeting_scheduling.models import MeetingScheduleModel
from ortools.sat.python import cp_model


def _pairs(count: int) -> int:
    return count * (count - 1) // 2


@dataclass
class MeetingContext:
    model: cp_model.CpModel
    problem: MeetingScheduleModel
    x: dict[tuple[int, int, int], Any]
    values: list[tuple[int, int]]
    grain_indices: list[int]
    days_at_index: dict[int, list[int]]
    larger_room_penalty: list[int]
    assignment_meetings: list[Any]
    required_counts: list[Counter]
    preferred_counts: list[Counter]
    _and_cache: dict[tuple[int, ...], Any] = field(default_factory=dict)

    def both(self, key: tuple[int, ...], left: Any, right: Any) -> Any:
        if key not in self._and_cache:
            value = self.model.new_bool_var("pair_" + "_".join(map(str, key)))
            self.model.add(value <= left)
            self.model.add(value <= right)
            self.model.add(value >= left + right - 1)
            self._and_cache[key] = value
        return self._and_cache[key]


class _UnaryPlugin(ConstraintPlugin[MeetingContext]):
    rule: ClassVar[str]

    def amount(
        self,
        context: MeetingContext,
        assignment: int,
        grain: int,
        room: int,
        rule: str,
    ) -> int:
        meeting = context.assignment_meetings[assignment]
        meeting_index = context.problem.meeting_assignments[assignment].meeting
        start = context.grain_indices[grain]
        last = start + meeting.duration_in_grains - 1
        if rule == "overtime":
            return last if last not in context.days_at_index else 0
        if rule == "capacity":
            return max(
                0,
                len(meeting.required_attendees)
                + len(meeting.preferred_attendees)
                - context.problem.rooms[room].capacity,
            )
        if rule == "same_day":
            if last not in context.days_at_index:
                return 0
            return len(context.days_at_index[last]) - context.days_at_index[last].count(
                context.problem.time_grains[grain].day_of_year
            )
        if rule == "required_self":
            return (
                sum(
                    _pairs(count)
                    for count in context.required_counts[meeting_index].values()
                )
                * meeting.duration_in_grains
            )
        if rule == "preferred_self":
            return (
                sum(
                    _pairs(count)
                    for count in context.preferred_counts[meeting_index].values()
                )
                * meeting.duration_in_grains
            )
        if rule == "mixed_self":
            required = context.required_counts[meeting_index]
            preferred = context.preferred_counts[meeting_index]
            return (
                sum(required[p] * preferred[p] for p in set(required) | set(preferred))
                * meeting.duration_in_grains
            )
        if rule == "early":
            return last
        return context.larger_room_penalty[room]

    def build_rule(
        self, context: MeetingContext, objective: ObjectiveBuilder, rule: str
    ) -> None:
        for assignment in range(len(context.assignment_meetings)):
            for grain, room in context.values:
                amount = self.amount(context, assignment, grain, room, rule)
                objective.add(
                    self.name,
                    amount * context.x[assignment, grain, room],
                    bound=abs(amount),
                )

    def build(self, context: MeetingContext, objective: ObjectiveBuilder) -> None:
        self.build_rule(context, objective, self.rule)


class AvoidOvertime(domain.AvoidOvertime, _UnaryPlugin):
    default_level = "hard"
    rule = "overtime"


class RequiredRoomCapacity(domain.RequiredRoomCapacity, _UnaryPlugin):
    default_level = "hard"
    rule = "capacity"


class StartAndEndOnSameDay(domain.StartAndEndOnSameDay, _UnaryPlugin):
    default_level = "hard"
    rule = "same_day"


class DoMeetingsAsSoonAsPossible(domain.DoMeetingsAsSoonAsPossible, _UnaryPlugin):
    default_level = "soft"
    rule = "early"


class AssignLargerRoomsFirst(domain.AssignLargerRoomsFirst, _UnaryPlugin):
    default_level = "soft"
    rule = "larger_room"


class _PairPlugin(ConstraintPlugin[MeetingContext]):
    rule: ClassVar[str]

    def weights(
        self, context: MeetingContext, left: int, right: int
    ) -> tuple[int, int, int, int]:
        ia = context.problem.meeting_assignments[left].meeting
        ib = context.problem.meeting_assignments[right].meeting
        ra, rb = context.required_counts[ia], context.required_counts[ib]
        pa, pb = context.preferred_counts[ia], context.preferred_counts[ib]
        required = sum(ra[p] * rb[p] for p in ra.keys() & rb.keys())
        preferred = sum(pa[p] * pb[p] for p in pa.keys() & pb.keys())
        mixed = sum(ra[p] * pb[p] for p in ra.keys() & pb.keys()) + sum(
            rb[p] * pa[p] for p in rb.keys() & pa.keys()
        )
        attendance = sum(
            (ra[p] + pa[p]) * (rb[p] + pb[p])
            for p in (ra.keys() | pa.keys()) & (rb.keys() | pb.keys())
        )
        return required, preferred, mixed, attendance

    def build_rule(
        self, context: MeetingContext, objective: ObjectiveBuilder, rule: str
    ) -> None:
        for left in range(len(context.assignment_meetings)):
            a = context.assignment_meetings[left]
            for right in range(left + 1, len(context.assignment_meetings)):
                b = context.assignment_meetings[right]
                required, preferred, mixed, attendance = self.weights(
                    context, left, right
                )
                for grain_a, room_a in context.values:
                    start_a = context.grain_indices[grain_a]
                    end_a = start_a + a.duration_in_grains
                    for grain_b, room_b in context.values:
                        start_b = context.grain_indices[grain_b]
                        end_b = start_b + b.duration_in_grains
                        overlap = max(0, min(end_a, end_b) - max(start_a, start_b))
                        consecutive = int(end_a == start_b or end_b == start_a)
                        stability = 0
                        if attendance and room_a != room_b:
                            ta = (
                                context.problem.time_grains[grain_a].day_of_year,
                                context.problem.time_grains[
                                    grain_a
                                ].starting_minute_of_day,
                            )
                            tb = (
                                context.problem.time_grains[grain_b].day_of_year,
                                context.problem.time_grains[
                                    grain_b
                                ].starting_minute_of_day,
                            )
                            if ta < tb and start_b - end_a <= 2:
                                stability = attendance
                            elif tb < ta and start_a - end_b <= 2:
                                stability = attendance
                        amounts = {
                            "room": overlap * int(room_a == room_b),
                            "required": overlap * required,
                            "preferred": overlap * preferred,
                            "mixed": overlap * mixed,
                            "overlap": overlap,
                            "break": consecutive,
                            "stability": stability,
                        }
                        amount = amounts[rule]
                        if not amount:
                            continue
                        both = context.both(
                            (left, grain_a, room_a, right, grain_b, room_b),
                            context.x[left, grain_a, room_a],
                            context.x[right, grain_b, room_b],
                        )
                        objective.add(self.name, amount * both, bound=amount)

    def build(self, context: MeetingContext, objective: ObjectiveBuilder) -> None:
        self.build_rule(context, objective, self.rule)


class RoomConflict(domain.RoomConflict, _PairPlugin):
    default_level = "hard"
    rule = "room"


class RequiredAttendanceConflict(
    domain.RequiredAttendanceConflict, _UnaryPlugin, _PairPlugin
):
    default_level = "hard"
    rule = "required_self"

    def build(self, context: MeetingContext, objective: ObjectiveBuilder) -> None:
        _UnaryPlugin.build_rule(self, context, objective, "required_self")
        _PairPlugin.build_rule(self, context, objective, "required")


class RequiredAndPreferredAttendanceConflict(
    domain.RequiredAndPreferredAttendanceConflict, _UnaryPlugin, _PairPlugin
):
    default_level = "medium"
    rule = "mixed_self"

    def build(self, context: MeetingContext, objective: ObjectiveBuilder) -> None:
        _UnaryPlugin.build_rule(self, context, objective, "mixed_self")
        _PairPlugin.build_rule(self, context, objective, "mixed")


class PreferredAttendanceConflict(
    domain.PreferredAttendanceConflict, _UnaryPlugin, _PairPlugin
):
    default_level = "medium"
    rule = "preferred_self"

    def build(self, context: MeetingContext, objective: ObjectiveBuilder) -> None:
        _UnaryPlugin.build_rule(self, context, objective, "preferred_self")
        _PairPlugin.build_rule(self, context, objective, "preferred")


class OneBreakBetweenConsecutiveMeetings(
    domain.OneBreakBetweenConsecutiveMeetings, _PairPlugin
):
    default_level = "soft"
    default_weight = 100
    rule = "break"


class OverlappingMeetings(domain.OverlappingMeetings, _PairPlugin):
    default_level = "soft"
    default_weight = 10
    rule = "overlap"


class RoomStability(domain.RoomStability, _PairPlugin):
    default_level = "soft"
    rule = "stability"


REGISTRY = ConstraintRegistry(
    [
        RoomConflict(),
        AvoidOvertime(),
        RequiredAttendanceConflict(),
        RequiredRoomCapacity(),
        StartAndEndOnSameDay(),
        RequiredAndPreferredAttendanceConflict(),
        PreferredAttendanceConflict(),
        DoMeetingsAsSoonAsPossible(),
        OneBreakBetweenConsecutiveMeetings(),
        OverlappingMeetings(),
        AssignLargerRoomsFirst(),
        RoomStability(),
    ]
)
