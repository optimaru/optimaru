"""Class-based CP-SAT plugins for meeting scheduling.

One plugin per engine constraint, subclassing the domain spec of the same name
(``meeting_scheduling.constraints``). The plugins carry only ``build()``: the
default level and weight of each one come from the engine's
``meeting_scheduling.constraint_info()`` (cpsat-common reads the row), so the
medium pair defaults to weight 1 on the CP-SAT medium level and
``consecutive_meetings_cost`` / ``meeting_overlap_cost`` to 100 / 10 on soft,
exactly as in C++.

The terms follow the engine's units, so a pick's ``offset`` / ``clip`` shape
them exactly as the engine does (``terms_per_unit``): the five per-meeting
constraints add one term per (grain, room) cell — the sum of every assignment
placed there, an empty cell (a constant 0) included — and the seven pair
constraints, global in the engine, add one term: their total. A constraint
picked several times adds one set of terms per pick, each with its own knobs.
"""

from collections import Counter
from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field
from itertools import chain
from typing import Any, ClassVar

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from meeting_scheduling import constraints as domain
from meeting_scheduling.models import MeetingScheduleModel
from ortools.sat.python import cp_model


def _pairs(count: int) -> int:
    return count * (count - 1) // 2


def _add_total(
    objective: ObjectiveBuilder, name: str, terms: Iterable[tuple[Any, int]]
) -> None:
    """Add a global constraint's pieces as ONE term, its total (a 0 included)."""
    pieces = []
    bound = 0
    for term, amount in terms:
        if amount:
            pieces.append(term)
            bound += amount
    objective.add(name, sum(pieces) if pieces else 0, bound=bound)


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
        if rule == "day_crossing":
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
        if rule == "end_time":
            return last
        return context.larger_room_penalty[room]

    terms_per_unit = True  # one term per (grain, room) cell, the engine's unit

    def cell_terms(
        self, context: MeetingContext, rule: str
    ) -> Iterator[tuple[Any, int]]:
        """``(term, bound)`` of every (grain, room) cell: the sum over the
        assignments placed there (a constant 0 for a cell nobody can cost)."""
        for grain, room in context.values:
            pieces = []
            bound = 0
            for assignment in range(len(context.assignment_meetings)):
                amount = self.amount(context, assignment, grain, room, rule)
                if amount:
                    pieces.append(amount * context.x[assignment, grain, room])
                    bound += abs(amount)
            yield (sum(pieces) if pieces else 0), bound

    def build(self, context: MeetingContext, objective: ObjectiveBuilder) -> None:
        for term, bound in self.cell_terms(context, self.rule):
            objective.add(self.name, term, bound=bound)


class OvertimeCost(domain.OvertimeCost, _UnaryPlugin):
    rule = "overtime"


class RoomCapacityCost(domain.RoomCapacityCost, _UnaryPlugin):
    rule = "capacity"


class DayCrossingCost(domain.DayCrossingCost, _UnaryPlugin):
    rule = "day_crossing"


class MeetingEndTimeCost(domain.MeetingEndTimeCost, _UnaryPlugin):
    rule = "end_time"


class LargerRoomCapacityCost(domain.LargerRoomCapacityCost, _UnaryPlugin):
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

    terms_per_unit = True  # global in the engine: one term, the total

    def pair_terms(
        self, context: MeetingContext, rule: str
    ) -> Iterator[tuple[Any, int]]:
        """``(term, bound)`` of every pair placement that costs something."""
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
                        room_change = 0
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
                                room_change = attendance
                            elif tb < ta and start_a - end_b <= 2:
                                room_change = attendance
                        amounts = {
                            "room": overlap * int(room_a == room_b),
                            "required": overlap * required,
                            "preferred": overlap * preferred,
                            "mixed": overlap * mixed,
                            "overlap": overlap,
                            "consecutive": consecutive,
                            "room_change": room_change,
                        }
                        amount = amounts[rule]
                        if not amount:
                            continue
                        both = context.both(
                            (left, grain_a, room_a, right, grain_b, room_b),
                            context.x[left, grain_a, room_a],
                            context.x[right, grain_b, room_b],
                        )
                        yield amount * both, amount

    def build(self, context: MeetingContext, objective: ObjectiveBuilder) -> None:
        _add_total(objective, self.name, self.pair_terms(context, self.rule))


class RoomConflictCost(domain.RoomConflictCost, _PairPlugin):
    rule = "room"


class RequiredAttendanceConflictCost(
    domain.RequiredAttendanceConflictCost, _UnaryPlugin, _PairPlugin
):
    rule = "required_self"

    def build(self, context: MeetingContext, objective: ObjectiveBuilder) -> None:
        # Global in the engine: the self term of every placement and the pair
        # terms make one total.
        _add_total(
            objective,
            self.name,
            chain(
                self.cell_terms(context, "required_self"),
                self.pair_terms(context, "required"),
            ),
        )


class RequiredAndPreferredAttendanceConflictCost(
    domain.RequiredAndPreferredAttendanceConflictCost, _UnaryPlugin, _PairPlugin
):
    rule = "mixed_self"

    def build(self, context: MeetingContext, objective: ObjectiveBuilder) -> None:
        # Global in the engine: the self term of every placement and the pair
        # terms make one total.
        _add_total(
            objective,
            self.name,
            chain(
                self.cell_terms(context, "mixed_self"),
                self.pair_terms(context, "mixed"),
            ),
        )


class PreferredAttendanceConflictCost(
    domain.PreferredAttendanceConflictCost, _UnaryPlugin, _PairPlugin
):
    rule = "preferred_self"

    def build(self, context: MeetingContext, objective: ObjectiveBuilder) -> None:
        # Global in the engine: the self term of every placement and the pair
        # terms make one total.
        _add_total(
            objective,
            self.name,
            chain(
                self.cell_terms(context, "preferred_self"),
                self.pair_terms(context, "preferred"),
            ),
        )


class ConsecutiveMeetingsCost(domain.ConsecutiveMeetingsCost, _PairPlugin):
    rule = "consecutive"


class MeetingOverlapCost(domain.MeetingOverlapCost, _PairPlugin):
    rule = "overlap"


class RoomChangeCost(domain.RoomChangeCost, _PairPlugin):
    rule = "room_change"


# Plugin order = the engine's list_constraints() order (the per-meeting
# constraints first, then the pair constraints), so catalog() lists the
# constraints exactly like constraint_info().
REGISTRY = ConstraintRegistry(
    [
        OvertimeCost(),
        RoomCapacityCost(),
        DayCrossingCost(),
        MeetingEndTimeCost(),
        LargerRoomCapacityCost(),
        RoomConflictCost(),
        RequiredAttendanceConflictCost(),
        RequiredAndPreferredAttendanceConflictCost(),
        PreferredAttendanceConflictCost(),
        ConsecutiveMeetingsCost(),
        MeetingOverlapCost(),
        RoomChangeCost(),
    ]
)
