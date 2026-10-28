"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 Person/TimeGrain/Room/Meeting/MeetingAssignment/MeetingSchedule 을
1:1 로 미러링한다. 사람/미팅/타임그레인/회의실은 리스트 인덱스로 참조한다
(Java 의 객체 참조 대신). 미팅과 배정은 1:1 이다 (quickstart 데모 모델).

점수 매핑 (Java HardMediumSoftScore -> 엔진 HardSoftScore):
``score_hard`` 는 Java hard 그대로, ``score_soft`` 는
``-(medium 벌점 * 1e6 + soft 벌점)`` — medium(필수+선호/선호 참석 충돌)이
soft 를 항상 지배하도록 1e6 오프셋으로 접었다::

    from meeting_scheduling.models import MeetingScheduleModel, demo_data

    model = demo_data()                          # Java DemoDataGenerator 재현
    clone = MeetingScheduleModel.model_validate_json(model.model_dump_json())
    schedule = clone.to_core()                   # pydantic -> 엔진

`pydantic` 은 선택 의존성이다 (`pip install meeting_scheduling[models]`).
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
    """참석자. 참조는 people 리스트 인덱스로 한다."""

    model_config = ConfigDict(extra="forbid")

    id: str
    full_name: str = ""


class TimeGrainModel(BaseModel):
    """15분 단위 타임그레인. grain_index 는 duration/overlap 이 계산되는
    단위 (Java grainIndex; 데모는 1-base 연속)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    grain_index: int
    day_of_year: int
    starting_minute_of_day: int = Field(ge=0)


class RoomModel(BaseModel):
    """회의실. capacity 가 required room capacity 하드 제약에 쓰인다."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str = ""
    capacity: int = 0


class MeetingModel(BaseModel):
    """미팅 (problem fact). required/preferred_attendees 는 people 인덱스
    리스트 — 원소 하나가 Java Attendance fact 하나다."""

    model_config = ConfigDict(extra="forbid")

    id: str
    topic: str = ""
    duration_in_grains: int = Field(default=1, ge=1)
    required_attendees: list[int] = Field(default_factory=list)
    preferred_attendees: list[int] = Field(default_factory=list)
    entire_group_meeting: bool = False


class MeetingAssignmentModel(BaseModel):
    """planning entity. grain_room 은 time_grain_pos * room_count +
    room_index 합성 변수 (None == 미배정); meeting 은 meetings 인덱스."""

    model_config = ConfigDict(extra="forbid")

    id: str
    meeting: int = Field(ge=0)
    grain_room: int | None = None
    pinned: bool = False


class MeetingScheduleModel(BaseModel):
    """planning solution 미러. score 는 읽기 전용 결과라 별도 필드로 둔다."""

    model_config = ConfigDict(extra="forbid")

    people: list[PersonModel]
    time_grains: list[TimeGrainModel]
    rooms: list[RoomModel]
    meetings: list[MeetingModel]
    meeting_assignments: list[MeetingAssignmentModel] = Field(default_factory=list)
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.MeetingSchedule":
        """pydantic -> 엔진 객체 (score 필드는 엔진이 다시 계산하므로 무시)."""
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
        """엔진 객체 -> pydantic (현재 배정과 score 포함)."""
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
    """Java quickstart 의 DemoDataGenerator 재현 (java.util.Random(0) 과 같은
    추첨; C++ `ms::buildDemoData` 를 그대로 노출): 사람 20, 타임그레인 160
    (4일 x 40), 회의실 3 (30/20/16석), 미팅 24 (8/12/16 그레인)."""
    return MeetingScheduleModel.from_core(_core.demo_data())
