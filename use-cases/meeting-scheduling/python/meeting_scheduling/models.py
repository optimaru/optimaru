"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Person/TimeGrain/Room/Meeting/MeetingAssignment/MeetingSchedule
1:1. People/meetings/time grains/rooms are referenced by list index (instead of
Java's object references). Meetings and assignments are 1:1 (the quickstart demo model).

Score mapping (Java HardMediumSoftScore -> engine HardSoftScore):
``score_hard`` is the Java hard as-is, ``score_soft`` is
``-(medium penalty * 1e6 + soft penalty)`` — folded with a 1e6 offset so medium
(required+preferred / preferred attendance conflicts) always dominates soft::

    from meeting_scheduling.models import MeetingScheduleModel, demo_data

    model = demo_data()                          # reproduces Java DemoDataGenerator
    clone = MeetingScheduleModel.model_validate_json(model.model_dump_json())
    schedule = clone.to_core()                   # pydantic -> engine

`pydantic` is an optional dependency (`pip install meeting_scheduling[models]`).
"""

from pydantic import BaseModel, ConfigDict, Field

from . import _core

__all__ = [
    "MeetingAssignmentModel",
    "MeetingModel",
    "MeetingScheduleModel",
    "PersonModel",
    "RoomModel",
    "TimeGrainModel",
    "demo_data",
]


class PersonModel(BaseModel):
    """An attendee. Referenced by index into the people list."""

    model_config = ConfigDict(extra="forbid")

    id: str
    full_name: str = ""


class TimeGrainModel(BaseModel):
    """A 15-minute time grain. grain_index is the unit duration/overlap is computed in
    (Java grainIndex; consecutive and 1-based in the demo)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    grain_index: int
    day_of_year: int
    starting_minute_of_day: int = Field(ge=0)


class RoomModel(BaseModel):
    """A meeting room. capacity feeds the required room capacity hard constraint."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str = ""
    capacity: int = 0


class MeetingModel(BaseModel):
    """A meeting (problem fact). required/preferred_attendees are lists of people
    indices — one element is one Java Attendance fact."""

    model_config = ConfigDict(extra="forbid")

    id: str
    topic: str = ""
    duration_in_grains: int = Field(default=1, ge=1)
    required_attendees: list[int] = Field(default_factory=list)
    preferred_attendees: list[int] = Field(default_factory=list)
    entire_group_meeting: bool = False


class MeetingAssignmentModel(BaseModel):
    """Planning entity. grain_room is the composite variable time_grain_pos * room_count +
    room_index (None == unassigned); meeting is a meetings index."""

    model_config = ConfigDict(extra="forbid")

    id: str
    meeting: int = Field(ge=0)
    grain_room: int | None = None
    pinned: bool = False


class MeetingScheduleModel(BaseModel):
    """Planning solution mirror. score is a read-only result, kept as a separate field."""

    model_config = ConfigDict(extra="forbid")

    people: list[PersonModel]
    time_grains: list[TimeGrainModel]
    rooms: list[RoomModel]
    meetings: list[MeetingModel]
    meeting_assignments: list[MeetingAssignmentModel] = Field(default_factory=list)
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.MeetingSchedule":
        """pydantic -> engine object (the score field is ignored; the engine recomputes it)."""
        return _core.MeetingSchedule(
            [_core.Person(p.id, p.full_name) for p in self.people],
            [_core.TimeGrain(g.id, g.grain_index, g.day_of_year, g.starting_minute_of_day) for g in self.time_grains],
            [_core.Room(r.id, r.name, r.capacity) for r in self.rooms],
            [
                _core.Meeting(
                    m.id,
                    topic=m.topic,
                    duration_in_grains=m.duration_in_grains,
                    required_attendees=list(m.required_attendees),
                    preferred_attendees=list(m.preferred_attendees),
                    entire_group_meeting=m.entire_group_meeting,
                )
                for m in self.meetings
            ],
            [
                _core.MeetingAssignment(
                    a.id,
                    a.meeting,
                    grain_room=-1 if a.grain_room is None else a.grain_room,
                    pinned=a.pinned,
                )
                for a in self.meeting_assignments
            ],
        )

    @classmethod
    def from_core(cls, schedule: "_core.MeetingSchedule") -> "MeetingScheduleModel":
        """engine object -> pydantic (including the current assignment and score)."""
        return cls(
            people=[PersonModel(id=p.id, full_name=p.full_name) for p in schedule.people],
            time_grains=[
                TimeGrainModel(
                    id=g.id,
                    grain_index=g.grain_index,
                    day_of_year=g.day_of_year,
                    starting_minute_of_day=g.starting_minute_of_day,
                )
                for g in schedule.time_grains
            ],
            rooms=[RoomModel(id=r.id, name=r.name, capacity=r.capacity) for r in schedule.rooms],
            meetings=[
                MeetingModel(
                    id=m.id,
                    topic=m.topic,
                    duration_in_grains=m.duration_in_grains,
                    required_attendees=list(m.required_attendees),
                    preferred_attendees=list(m.preferred_attendees),
                    entire_group_meeting=m.entire_group_meeting,
                )
                for m in schedule.meetings
            ],
            meeting_assignments=[
                MeetingAssignmentModel(
                    id=a.id,
                    meeting=a.meeting,
                    grain_room=None if a.grain_room < 0 else a.grain_room,
                    pinned=a.pinned,
                )
                for a in schedule.meeting_assignments
            ],
            score_hard=schedule.score.hard,
            score_soft=schedule.score.soft,
        )


def demo_data() -> MeetingScheduleModel:
    """Reproduces the Java quickstart DemoDataGenerator (the same draws as
    java.util.Random(0); exposes C++ `ms::buildDemoData` as-is): 20 people, 160 time
    grains (4 days x 40), 3 rooms (30/20/16 seats), 24 meetings (8/12/16 grains)."""
    return MeetingScheduleModel.from_core(_core.demo_data())
